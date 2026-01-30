/**
 * @file Allocators.cpp
 * @brief Memory allocators implementation
 */

#include "Core/Memory/Allocators.h"
#include "Core/Log.h"
#include <cstring>
#include <algorithm>

#ifdef _WIN32
#include <malloc.h>  // For _aligned_malloc/_aligned_free on Windows
#endif

namespace RVX
{

// Platform-specific aligned allocation
static void* AlignedAlloc(size_t alignment, size_t size)
{
#ifdef _WIN32
    return _aligned_malloc(size, alignment);
#else
    return std::aligned_alloc(alignment, size);
#endif
}

static void AlignedFree(void* ptr)
{
#ifdef _WIN32
    _aligned_free(ptr);
#else
    std::free(ptr);
#endif
}

// ============================================================================
// LinearAllocator
// ============================================================================

LinearAllocator::LinearAllocator(size_t capacity)
    : m_capacity(capacity)
    , m_offset(0)
{
    m_memory = static_cast<uint8*>(std::malloc(capacity));
}

LinearAllocator::~LinearAllocator()
{
    if (m_memory)
    {
        std::free(m_memory);
    }
}

LinearAllocator::LinearAllocator(LinearAllocator&& other) noexcept
    : m_memory(other.m_memory)
    , m_capacity(other.m_capacity)
    , m_offset(other.m_offset)
{
    other.m_memory = nullptr;
    other.m_capacity = 0;
    other.m_offset = 0;
}

LinearAllocator& LinearAllocator::operator=(LinearAllocator&& other) noexcept
{
    if (this != &other)
    {
        if (m_memory)
        {
            std::free(m_memory);
        }
        m_memory = other.m_memory;
        m_capacity = other.m_capacity;
        m_offset = other.m_offset;
        other.m_memory = nullptr;
        other.m_capacity = 0;
        other.m_offset = 0;
    }
    return *this;
}

void* LinearAllocator::Allocate(size_t size, size_t alignment)
{
    // Align offset
    size_t alignedOffset = (m_offset + alignment - 1) & ~(alignment - 1);
    
    if (alignedOffset + size > m_capacity)
    {
        return nullptr;  // Out of memory
    }

    void* ptr = m_memory + alignedOffset;
    m_offset = alignedOffset + size;
    return ptr;
}

void LinearAllocator::Reset()
{
    m_offset = 0;
}

bool LinearAllocator::CanAllocate(size_t size, size_t alignment) const
{
    size_t alignedOffset = (m_offset + alignment - 1) & ~(alignment - 1);
    return alignedOffset + size <= m_capacity;
}

// ============================================================================
// PoolAllocator
// ============================================================================

PoolAllocator::PoolAllocator(size_t objectSize, size_t objectCount, size_t alignment)
    : m_objectSize(std::max(objectSize, sizeof(FreeNode)))
    , m_objectCount(objectCount)
    , m_alignment(alignment)
    , m_allocatedCount(0)
{
    // Ensure object size is aligned
    m_objectSize = (m_objectSize + alignment - 1) & ~(alignment - 1);
    
    size_t totalSize = m_objectSize * objectCount;
    m_memory = static_cast<uint8*>(AlignedAlloc(alignment, totalSize));
    
    BuildFreeList();
}

PoolAllocator::~PoolAllocator()
{
    if (m_memory)
    {
        AlignedFree(m_memory);
    }
}

void PoolAllocator::BuildFreeList()
{
    m_freeList = nullptr;
    
    for (size_t i = 0; i < m_objectCount; ++i)
    {
        FreeNode* node = reinterpret_cast<FreeNode*>(m_memory + i * m_objectSize);
        node->next = m_freeList;
        m_freeList = node;
    }
}

void* PoolAllocator::Allocate(size_t size, size_t alignment)
{
    (void)size;
    (void)alignment;
    
    if (!m_freeList)
    {
        return nullptr;  // Pool exhausted
    }

    FreeNode* node = m_freeList;
    m_freeList = m_freeList->next;
    m_allocatedCount++;
    
    return node;
}

void PoolAllocator::Free(void* ptr)
{
    if (!ptr) return;
    
    // Verify ptr is within our memory range
    uint8* p = static_cast<uint8*>(ptr);
    if (p < m_memory || p >= m_memory + m_objectCount * m_objectSize)
    {
        return;  // Invalid pointer
    }

    FreeNode* node = static_cast<FreeNode*>(ptr);
    node->next = m_freeList;
    m_freeList = node;
    m_allocatedCount--;
}

void PoolAllocator::Reset()
{
    m_allocatedCount = 0;
    BuildFreeList();
}

// ============================================================================
// FrameAllocator
// ============================================================================

FrameAllocator& FrameAllocator::Get()
{
    static FrameAllocator instance;
    return instance;
}

void FrameAllocator::Initialize(size_t capacityPerFrame)
{
    m_capacityPerFrame = capacityPerFrame;
    
    for (int i = 0; i < kBufferCount; ++i)
    {
        m_allocators[i] = std::make_unique<LinearAllocator>(capacityPerFrame);
    }
    
    m_currentBuffer = 0;
    m_frameIndex = 0;
}

void FrameAllocator::Shutdown()
{
    for (int i = 0; i < kBufferCount; ++i)
    {
        m_allocators[i].reset();
    }
}

void* FrameAllocator::Allocate(size_t size, size_t alignment)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    
    if (!m_allocators[m_currentBuffer])
    {
        return nullptr;
    }
    
    return m_allocators[m_currentBuffer]->Allocate(size, alignment);
}

void FrameAllocator::NextFrame()
{
    m_frameIndex++;
    m_currentBuffer = (m_currentBuffer + 1) % kBufferCount;
    
    if (m_allocators[m_currentBuffer])
    {
        m_allocators[m_currentBuffer]->Reset();
    }
}

size_t FrameAllocator::GetUsedMemory() const
{
    if (m_allocators[m_currentBuffer])
    {
        return m_allocators[m_currentBuffer]->GetUsedMemory();
    }
    return 0;
}

// ============================================================================
// StackAllocator
// ============================================================================

StackAllocator::StackAllocator(size_t capacity)
    : m_capacity(capacity)
    , m_offset(0)
{
    m_memory = static_cast<uint8*>(std::malloc(capacity));
}

StackAllocator::~StackAllocator()
{
    if (m_memory)
    {
        std::free(m_memory);
    }
}

void* StackAllocator::Allocate(size_t size, size_t alignment)
{
    size_t alignedOffset = (m_offset + alignment - 1) & ~(alignment - 1);
    
    if (alignedOffset + size > m_capacity)
    {
        return nullptr;
    }

    void* ptr = m_memory + alignedOffset;
    m_offset = alignedOffset + size;
    return ptr;
}

void StackAllocator::Reset()
{
    m_offset = 0;
}

void StackAllocator::FreeToMarker(Marker marker)
{
    if (marker <= m_offset)
    {
        m_offset = marker;
    }
}

// ============================================================================
// MemoryTracker
// ============================================================================

MemoryTracker& MemoryTracker::Get()
{
    static MemoryTracker instance;
    return instance;
}

void MemoryTracker::TrackAllocation(void* ptr, size_t size, const char* file, int line)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    
    m_allocations[ptr] = {size, file, line};
    m_totalAllocated += size;
    m_allocationCount++;
    m_peakAllocated = std::max(m_peakAllocated, m_totalAllocated);
}

void MemoryTracker::TrackDeallocation(void* ptr)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    
    auto it = m_allocations.find(ptr);
    if (it != m_allocations.end())
    {
        m_totalAllocated -= it->second.size;
        m_allocations.erase(it);
    }
}

void MemoryTracker::PrintSummary() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    
    RVX_CORE_INFO("Memory Tracker Summary:");
    RVX_CORE_INFO("  Total Allocated: {} bytes", m_totalAllocated);
    RVX_CORE_INFO("  Peak Allocated: {} bytes", m_peakAllocated);
    RVX_CORE_INFO("  Allocation Count: {}", m_allocationCount);
    RVX_CORE_INFO("  Outstanding Allocations: {}", m_allocations.size());
}

bool MemoryTracker::CheckForLeaks() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    
    if (m_allocations.empty())
    {
        RVX_CORE_INFO("No memory leaks detected.");
        return true;
    }

    RVX_CORE_WARN("Memory leaks detected: {} allocations", m_allocations.size());
    
    for (const auto& [ptr, info] : m_allocations)
    {
        RVX_CORE_WARN("  Leak: {} bytes at {} ({}:{})", 
                      info.size, ptr, info.file, info.line);
    }
    
    return false;
}

} // namespace RVX
