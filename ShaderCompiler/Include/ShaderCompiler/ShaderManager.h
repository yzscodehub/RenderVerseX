#pragma once

#include "ShaderCompiler/ShaderCompiler.h"
#include "ShaderCompiler/ShaderTypes.h"
#include "ShaderCompiler/ShaderCompileService.h"
#include "ShaderCompiler/ShaderCacheManager.h"
#include "ShaderCompiler/ShaderPermutation.h"
#include "ShaderCompiler/ShaderHotReloader.h"
#include "RHI/RHIShader.h"
#include <memory>
#include <unordered_map>
#include <mutex>

namespace RVX
{
    class IRHIDevice;

    // =========================================================================
    // ShaderManager Configuration
    // =========================================================================
    struct ShaderManagerConfig
    {
        // Cache configuration
        std::filesystem::path cacheDirectory;
        uint64 maxCacheSizeBytes = 512 * 1024 * 1024;
        bool enableMemoryCache = true;
        bool enableDiskCache = true;

        // Compilation configuration
        uint32 maxConcurrentCompiles = 4;
        bool enableAsyncCompile = true;

        // Hot reload configuration
        bool enableHotReload = false;  // Disabled by default, enable in editor/dev mode
        std::vector<std::filesystem::path> shaderDirectories;

        // Statistics
        bool enableStatistics = true;
    };

    // =========================================================================
    // ShaderManager Statistics
    // =========================================================================
    struct ShaderManagerStats
    {
        // Compile statistics
        ShaderCompileService::Statistics compileStats;

        // Cache statistics
        ShaderCacheManager::Statistics cacheStats;

        // Variant statistics
        uint32 totalRegisteredShaders = 0;
        uint32 totalCompiledVariants = 0;
        uint32 pendingCompiles = 0;

        // Hot reload statistics
        uint32 reloadCount = 0;
        uint32 reloadSuccessCount = 0;
        uint32 reloadFailureCount = 0;
    };

    // =========================================================================
    // Shader Load Description (backward compatible)
    // =========================================================================
    struct ShaderLoadDesc
    {
        std::string path;
        std::string entryPoint;
        RHIShaderStage stage = RHIShaderStage::None;
        RHIBackendType backend = RHIBackendType::DX12;
        std::string targetProfile;
        std::vector<ShaderMacro> defines;
        bool enableDebugInfo = false;
        bool enableOptimization = true;
    };

    // =========================================================================
    // Shader Load Result
    // =========================================================================
    struct ShaderLoadResult
    {
        RHIShaderRef shader;
        ShaderCompileResult compileResult;
    };

    // =========================================================================
    // ShaderManager - Unified shader management facade
    // =========================================================================
    class ShaderManager
    {
    public:
        // =====================================================================
        // Construction
        // =====================================================================

        /** @brief Create ShaderManager with configuration */
        explicit ShaderManager(const ShaderManagerConfig& config = {});

        /** @brief Legacy constructor for backward compatibility */
        [[deprecated("Use ShaderManager(const ShaderManagerConfig&) instead")]]
        explicit ShaderManager(std::unique_ptr<IShaderCompiler> compiler);

        ~ShaderManager();

        // Non-copyable
        ShaderManager(const ShaderManager&) = delete;
        ShaderManager& operator=(const ShaderManager&) = delete;

        // =====================================================================
        // Synchronous Loading (backward compatible)
        // =====================================================================

        /** @brief Load shader from file (synchronous, may block) */
        ShaderLoadResult LoadFromFile(IRHIDevice* device, const ShaderLoadDesc& desc);

        /** @brief Load shader from source (synchronous) */
        ShaderLoadResult LoadFromSource(
            IRHIDevice* device,
            const ShaderLoadDesc& desc,
            const std::string& source);

        // =====================================================================
        // Asynchronous Loading
        // =====================================================================

        /** @brief Load shader from file asynchronously */
        CompileHandle LoadFromFileAsync(
            IRHIDevice* device,
            const ShaderLoadDesc& desc,
            LoadCallback onComplete);

        /** @brief Load shader from source asynchronously */
        CompileHandle LoadFromSourceAsync(
            IRHIDevice* device,
            const ShaderLoadDesc& desc,
            const std::string& source,
            LoadCallback onComplete);

        /** @brief Wait for async load to complete */
        ShaderLoadResult WaitForLoad(CompileHandle handle);

        /** @brief Check if async load is complete */
        bool IsLoadComplete(CompileHandle handle) const;

        // =====================================================================
        // Variant System
        // =====================================================================

        /** @brief Register shader variant space */
        void RegisterShaderVariants(
            const std::string& shaderPath,
            const ShaderPermutationSpace& space,
            const ShaderLoadDesc& baseDesc);

        /** @brief Get shader variant */
        RHIShaderRef GetShaderVariant(
            IRHIDevice* device,
            const std::string& shaderPath,
            const std::vector<ShaderMacro>& defines);

        /** @brief Prewarm variants */
        void PrewarmVariants(
            IRHIDevice* device,
            const std::string& shaderPath,
            const std::vector<std::vector<ShaderMacro>>& variants);

        // =====================================================================
        // Hot Reload
        // =====================================================================

        /** @brief Enable hot reload (typically in editor mode) */
        void EnableHotReload();

        /** @brief Disable hot reload */
        void DisableHotReload();

        /** @brief Check if hot reload is enabled */
        bool IsHotReloadEnabled() const;

        /** @brief Register hot reload callback */
        void SetHotReloadCallback(ShaderReloadCallback callback);

        /** @brief Add shader watch directory */
        void AddShaderWatchDirectory(const std::filesystem::path& dir);

        // =====================================================================
        // Update (call each frame)
        // =====================================================================

        /** @brief Update, processes hot reload etc. */
        void Update();

        // =====================================================================
        // Cache Management
        // =====================================================================

        /** @brief Clear memory cache */
        void ClearCache();

        /** @brief Clear memory cache (explicit name) */
        void ClearMemoryCache();

        /** @brief Clear disk cache */
        void ClearDiskCache();

        /** @brief Invalidate specific shader cache */
        void InvalidateShader(const std::string& shaderPath);

        // =====================================================================
        // Statistics
        // =====================================================================

        /** @brief Get statistics */
        ShaderManagerStats GetStats() const;

        /** @brief Reset statistics */
        void ResetStats();

        // =====================================================================
        // Access Internal Services (advanced usage)
        // =====================================================================

        ShaderCompileService* GetCompileService() { return m_compileService.get(); }
        ShaderCacheManager* GetCacheManager() { return m_cacheManager.get(); }
        ShaderPermutationSystem* GetPermutationSystem() { return m_permutationSystem.get(); }
        ShaderHotReloader* GetHotReloader() { return m_hotReloader.get(); }

    private:
        // =====================================================================
        // Internal Implementation
        // =====================================================================
        uint64 BuildCacheKey(const ShaderLoadDesc& desc, uint64 sourceHash) const;
        bool LoadFile(const std::string& path, std::string& outSource) const;
        void Initialize(const ShaderManagerConfig& config);
        ShaderPermutationLoadDesc ConvertToPermutationDesc(const ShaderLoadDesc& desc) const;

        ShaderManagerConfig m_config;

        std::unique_ptr<ShaderCompileService> m_compileService;
        std::unique_ptr<ShaderCacheManager> m_cacheManager;
        std::unique_ptr<ShaderPermutationSystem> m_permutationSystem;
        std::unique_ptr<ShaderHotReloader> m_hotReloader;

        // Legacy cache for backward compatibility
        mutable std::mutex m_legacyCacheMutex;
        std::unordered_map<uint64, RHIShaderRef> m_legacyCache;

        // Async load task mapping
        mutable std::mutex m_loadTasksMutex;
        std::unordered_map<CompileHandle, ShaderLoadDesc> m_loadTasks;
    };

} // namespace RVX
