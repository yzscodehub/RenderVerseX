#pragma once

#include "DX12Common.h"
#include "RHI/RHIPipeline.h"
#include <mutex>
#include <string>
#include <vector>
#include <unordered_map>

namespace RVX
{
    class DX12Device;

    // =============================================================================
    // DX12 Pipeline Cache
    // Uses ID3D12PipelineLibrary for disk-based PSO caching
    // =============================================================================
    class DX12PipelineCache
    {
    public:
        DX12PipelineCache() = default;
        ~DX12PipelineCache();

        // =========================================================================
        // Lifecycle
        // =========================================================================
        
        /**
         * @brief Initialize the pipeline cache
         * @param device The DX12 device
         * @param cachePath Path to the cache file (optional, empty = memory-only cache)
         * @return true if initialization succeeded
         */
        bool Initialize(DX12Device* device, const std::string& cachePath = "");
        
        /**
         * @brief Shutdown and save cache to disk
         */
        void Shutdown();

        // =========================================================================
        // PSO Management
        // =========================================================================
        
        /**
         * @brief Get or create a graphics PSO
         * @param name Unique name for the PSO (used as cache key)
         * @param desc Pipeline description
         * @param rootSig Root signature to use
         * @return The pipeline state object, or nullptr on failure
         */
        ComPtr<ID3D12PipelineState> GetOrCreateGraphicsPSO(
            const std::wstring& name,
            const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc);
        
        /**
         * @brief Get or create a compute PSO
         * @param name Unique name for the PSO (used as cache key)
         * @param desc Pipeline description
         * @return The pipeline state object, or nullptr on failure
         */
        ComPtr<ID3D12PipelineState> GetOrCreateComputePSO(
            const std::wstring& name,
            const D3D12_COMPUTE_PIPELINE_STATE_DESC& desc);

        /**
         * @brief Save the cache to disk
         * @return true if save succeeded
         */
        bool SaveToFile();

        /**
         * @brief Get cache statistics
         */
        struct CacheStats
        {
            uint32 hitCount = 0;
            uint32 missCount = 0;
            uint32 totalPSOs = 0;
        };
        CacheStats GetStats() const { return m_stats; }

    private:
        bool LoadFromFile();
        std::wstring GeneratePSOName(const char* prefix, size_t hash);

        DX12Device* m_device = nullptr;
        std::string m_cachePath;
        
        // ID3D12PipelineLibrary for caching
        ComPtr<ID3D12PipelineLibrary1> m_pipelineLibrary;
        
        // In-memory cache for quick lookup
        std::unordered_map<std::wstring, ComPtr<ID3D12PipelineState>> m_psoCache;
        std::mutex m_cacheMutex;
        
        // Cache file data
        std::vector<uint8> m_cacheData;
        bool m_dirty = false;
        
        // Statistics
        mutable CacheStats m_stats;
    };

} // namespace RVX
