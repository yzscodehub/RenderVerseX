#pragma once

#include "DX12Common.h"
#include <mutex>
#include <queue>
#include <vector>
#include <array>

namespace RVX
{
    class DX12Device;

    // =============================================================================
    // DX12 Command Allocator Pool
    // Manages reusable command allocators per queue type
    // =============================================================================
    class DX12CommandAllocatorPool
    {
    public:
        DX12CommandAllocatorPool() = default;
        ~DX12CommandAllocatorPool() = default;

        // =========================================================================
        // Lifecycle
        // =========================================================================
        
        /**
         * @brief Initialize the allocator pool
         * @param device The DX12 device
         */
        void Initialize(DX12Device* device);
        
        /**
         * @brief Shutdown and release all allocators
         */
        void Shutdown();

        // =========================================================================
        // Allocator Management
        // =========================================================================
        
        /**
         * @brief Acquire an available command allocator
         * @param type The command list type (Direct, Compute, Copy)
         * @return A ready-to-use allocator, or nullptr on failure
         */
        ComPtr<ID3D12CommandAllocator> Acquire(D3D12_COMMAND_LIST_TYPE type);
        
        /**
         * @brief Release an allocator back to the pool
         * @param allocator The allocator to release
         * @param type The command list type
         * @param fenceValue The fence value that signals when this allocator's work is complete
         */
        void Release(ComPtr<ID3D12CommandAllocator> allocator, 
                     D3D12_COMMAND_LIST_TYPE type, 
                     uint64 fenceValue);
        
        /**
         * @brief Recycle allocators whose work has completed
         * @param completedFenceValue Current completed fence value
         */
        void Tick(uint64 completedFenceValue);

        /**
         * @brief Get pool statistics
         */
        struct PoolStats
        {
            uint32 totalAllocators = 0;
            uint32 availableAllocators = 0;
            uint32 pendingAllocators = 0;
        };
        PoolStats GetStats() const;

    private:
        static constexpr uint32 TYPE_COUNT = 3;  // Direct, Compute, Copy
        
        static uint32 TypeToIndex(D3D12_COMMAND_LIST_TYPE type)
        {
            switch (type)
            {
                case D3D12_COMMAND_LIST_TYPE_DIRECT:  return 0;
                case D3D12_COMMAND_LIST_TYPE_COMPUTE: return 1;
                case D3D12_COMMAND_LIST_TYPE_COPY:    return 2;
                default: return 0;
            }
        }

        struct PendingAllocator
        {
            ComPtr<ID3D12CommandAllocator> allocator;
            uint64 fenceValue;
        };

        DX12Device* m_device = nullptr;
        
        std::array<std::vector<ComPtr<ID3D12CommandAllocator>>, TYPE_COUNT> m_availableAllocators;
        std::array<std::queue<PendingAllocator>, TYPE_COUNT> m_pendingAllocators;
        std::mutex m_mutex;
    };

} // namespace RVX
