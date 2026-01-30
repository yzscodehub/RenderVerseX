#pragma once

#include "ShaderCompiler/ShaderCacheFormat.h"
#include "ShaderCompiler/ShaderSourceInfo.h"
#include "ShaderCompiler/ShaderReflection.h"
#include "ShaderCompiler/ShaderCompiler.h"
#include <filesystem>
#include <optional>
#include <shared_mutex>
#include <unordered_map>

namespace RVX
{
    // =========================================================================
    // Cache Entry
    // =========================================================================
    struct ShaderCacheEntry
    {
        std::vector<uint8> bytecode;
        ShaderReflection reflection;
        ShaderSourceInfo sourceInfo;
        uint64 timestamp = 0;

        // Backend-specific data
        std::string mslSource;         // Metal
        std::string mslEntryPoint;
        std::string glslSource;        // OpenGL
        uint32 glslVersion = 450;

        // Metadata
        RHIBackendType backend = RHIBackendType::None;
        RHIShaderStage stage = RHIShaderStage::None;
        bool debugInfo = false;
        bool optimized = true;
    };

    // =========================================================================
    // Shader Cache Manager
    // =========================================================================
    class ShaderCacheManager
    {
    public:
        // =====================================================================
        // Configuration
        // =====================================================================
        struct Config
        {
            std::filesystem::path cacheDirectory;
            uint64 maxCacheSizeBytes = 512 * 1024 * 1024; // 512 MB
            bool enableMemoryCache = true;
            bool enableDiskCache = true;
            bool validateOnLoad = true;  // Validate dependencies on load
        };

        // =====================================================================
        // Construction
        // =====================================================================
        explicit ShaderCacheManager(const Config& config = {});
        ~ShaderCacheManager() = default;

        // Non-copyable
        ShaderCacheManager(const ShaderCacheManager&) = delete;
        ShaderCacheManager& operator=(const ShaderCacheManager&) = delete;

        // =====================================================================
        // Cache Operations
        // =====================================================================

        /** @brief Load cache entry */
        std::optional<ShaderCacheEntry> Load(uint64 key);

        /** @brief Save cache entry */
        void Save(uint64 key, const ShaderCacheEntry& entry);

        /** @brief Check if cache is valid (dependencies unchanged) */
        bool IsValid(uint64 key, const ShaderSourceInfo& currentInfo);

        /** @brief Invalidate specific cache entry */
        void Invalidate(uint64 key);

        /** @brief Invalidate all cache entries */
        void InvalidateAll();

        /** @brief Clear memory cache */
        void ClearMemoryCache();

        // =====================================================================
        // Cache Directory Management
        // =====================================================================

        /** @brief Set cache directory */
        void SetCacheDirectory(const std::filesystem::path& dir);

        /** @brief Get cache directory */
        const std::filesystem::path& GetCacheDirectory() const { return m_config.cacheDirectory; }

        /** @brief Prune old cache files */
        void PruneCache(uint64 maxAgeSeconds = 7 * 24 * 3600); // Default 7 days

        /** @brief Enforce cache size limit */
        void EnforceSizeLimit();

        /** @brief Get total disk cache size */
        uint64 GetDiskCacheSize() const;

        // =====================================================================
        // Statistics
        // =====================================================================
        struct Statistics
        {
            uint64 memoryHits = 0;
            uint64 diskHits = 0;
            uint64 misses = 0;
            uint64 invalidations = 0;
            uint64 memoryCacheSize = 0;
            uint64 diskCacheSize = 0;

            float GetHitRate() const
            {
                uint64 total = memoryHits + diskHits + misses;
                return total > 0 ? static_cast<float>(memoryHits + diskHits) / total : 0.0f;
            }

            void Reset()
            {
                memoryHits = 0;
                diskHits = 0;
                misses = 0;
                invalidations = 0;
                memoryCacheSize = 0;
                diskCacheSize = 0;
            }
        };

        const Statistics& GetStatistics() const { return m_stats; }
        void ResetStatistics() { m_stats.Reset(); }

    private:
        // =====================================================================
        // Internal Implementation
        // =====================================================================
        std::filesystem::path GetCachePath(uint64 key) const;
        bool LoadFromDisk(uint64 key, ShaderCacheEntry& entry);
        void SaveToDisk(uint64 key, const ShaderCacheEntry& entry);
        bool ValidateHeader(const ShaderCacheHeader& header) const;
        void SerializeReflection(const ShaderReflection& reflection, std::vector<uint8>& out) const;
        ShaderReflection DeserializeReflection(const uint8* data, size_t size) const;
        uint64 ComputeContentHash(const ShaderCacheEntry& entry) const;

        Config m_config;

        // Memory cache
        mutable std::shared_mutex m_cacheMutex;
        std::unordered_map<uint64, ShaderCacheEntry> m_memoryCache;

        // Statistics
        mutable std::mutex m_statsMutex;
        Statistics m_stats;
    };

} // namespace RVX
