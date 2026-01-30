#pragma once

#include "ShaderCompiler/ShaderTypes.h"
#include "ShaderCompiler/ShaderPermutation.h"
#include "RHI/RHIShader.h"
#include <filesystem>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <mutex>
#include <thread>
#include <atomic>

namespace RVX
{
    class ShaderCompileService;
    class ShaderCacheManager;
    class IRHIDevice;

    // =========================================================================
    // File Change Event
    // =========================================================================
    struct ShaderFileChange
    {
        std::string path;
        enum class Type : uint8 { Modified, Created, Deleted } type;
        uint64 timestamp;
    };

    // =========================================================================
    // Reload Information
    // =========================================================================
    struct ShaderReloadInfo
    {
        std::string shaderPath;
        RHIShaderRef oldShader;
        RHIShaderRef newShader;
        bool success;
        std::string errorMessage;
    };

    using ShaderReloadCallback = std::function<void(const ShaderReloadInfo&)>;

    // =========================================================================
    // Shader Hot Reloader
    // =========================================================================
    class ShaderHotReloader
    {
    public:
        // =====================================================================
        // Configuration
        // =====================================================================
        struct Config
        {
            std::vector<std::filesystem::path> watchDirectories;
            std::vector<std::string> watchExtensions = {".hlsl", ".hlsli", ".ush", ".usf", ".h"};
            uint32 pollIntervalMs = 100;      // Polling interval
            uint32 debounceMs = 200;          // Debounce delay
            bool enabled = true;
        };

        // =====================================================================
        // Construction
        // =====================================================================
        ShaderHotReloader(
            ShaderCompileService* compileService,
            ShaderCacheManager* cacheManager,
            const Config& config = {});

        ~ShaderHotReloader();

        // Non-copyable
        ShaderHotReloader(const ShaderHotReloader&) = delete;
        ShaderHotReloader& operator=(const ShaderHotReloader&) = delete;

        // =====================================================================
        // Enable/Disable
        // =====================================================================

        void Enable();
        void Disable();
        bool IsEnabled() const { return m_enabled.load(); }

        // =====================================================================
        // Shader Registration
        // =====================================================================

        /** @brief Register shader for hot reload */
        void RegisterShader(
            IRHIDevice* device,
            const std::string& shaderPath,
            RHIShaderRef shader,
            const ShaderPermutationLoadDesc& loadDesc,
            ShaderReloadCallback callback = nullptr);

        /** @brief Unregister shader */
        void UnregisterShader(const std::string& shaderPath);

        /** @brief Unregister specific shader instance */
        void UnregisterShaderInstance(RHIShaderRef shader);

        // =====================================================================
        // Update
        // =====================================================================

        /** @brief Call each frame to process file changes */
        void Update();

        /** @brief Force reload a specific shader */
        void ForceReload(const std::string& shaderPath);

        /** @brief Force reload all shaders */
        void ForceReloadAll();

        // =====================================================================
        // Watch Directories
        // =====================================================================

        void AddWatchDirectory(const std::filesystem::path& dir);
        void RemoveWatchDirectory(const std::filesystem::path& dir);
        void ClearWatchDirectories();

        // =====================================================================
        // Global Callback
        // =====================================================================

        void SetGlobalReloadCallback(ShaderReloadCallback callback);

        // =====================================================================
        // Statistics
        // =====================================================================
        struct Statistics
        {
            uint32 reloadCount = 0;
            uint32 successCount = 0;
            uint32 failureCount = 0;
            uint32 watchedShaderCount = 0;
        };

        const Statistics& GetStatistics() const { return m_stats; }

    private:
        // =====================================================================
        // Internal Structures
        // =====================================================================
        struct WatchedShader
        {
            std::string path;
            ShaderPermutationLoadDesc loadDesc;
            std::vector<RHIShaderRef> instances;
            std::vector<ShaderReloadCallback> callbacks;
            std::unordered_set<std::string> dependencies; // include files
            uint64 lastModifiedTime = 0;
            IRHIDevice* device = nullptr;
        };

        struct PendingChange
        {
            std::string path;
            uint64 timestamp;
        };

        struct FileInfo
        {
            uint64 lastWriteTime;
            uint64 size;
        };

        void WatcherThread();
        void ProcessPendingChanges();
        void ReloadShader(WatchedShader& shader);
        std::vector<std::string> GetAffectedShaders(const std::string& changedFile);
        uint64 GetFileModificationTime(const std::filesystem::path& path) const;
        void ScanDirectory(const std::filesystem::path& dir);
        bool ShouldWatch(const std::filesystem::path& path) const;

        Config m_config;
        ShaderCompileService* m_compileService;
        ShaderCacheManager* m_cacheManager;

        // Watch state
        std::atomic<bool> m_enabled{false};
        std::atomic<bool> m_shutdown{false};
        std::thread m_watcherThread;

        // Registered shaders
        mutable std::mutex m_shadersMutex;
        std::unordered_map<std::string, WatchedShader> m_watchedShaders;

        // File state tracking
        mutable std::mutex m_filesMutex;
        std::unordered_map<std::string, FileInfo> m_trackedFiles;

        // Pending changes (for debouncing)
        std::mutex m_pendingMutex;
        std::unordered_map<std::string, PendingChange> m_pendingChanges;

        // Global callback
        ShaderReloadCallback m_globalCallback;

        // Statistics
        Statistics m_stats;
    };

} // namespace RVX
