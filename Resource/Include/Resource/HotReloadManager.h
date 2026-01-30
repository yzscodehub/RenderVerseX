#pragma once

/**
 * @file HotReloadManager.h
 * @brief Hot-reload manager for automatic resource updating
 * 
 * Integrates FileWatcher with ResourceManager to provide:
 * - Automatic resource reloading when files change
 * - Version tracking for resources
 * - Reload callbacks for notification
 * - Batched updates to avoid thrashing
 */

#include "Resource/FileWatcher.h"
#include "Resource/IResource.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <functional>
#include <memory>
#include <mutex>
#include <atomic>

namespace RVX::Resource
{
    // Forward declarations
    class ResourceManager;

    /**
     * @brief Resource version info for tracking changes
     */
    struct ResourceVersion
    {
        ResourceId resourceId = InvalidResourceId;
        std::string path;
        uint32_t version = 0;
        std::filesystem::file_time_type lastModified;
        bool pendingReload = false;
    };

    /**
     * @brief Reload event data
     */
    struct ReloadEvent
    {
        ResourceId resourceId;
        std::string path;
        IResource* oldResource;
        IResource* newResource;
        uint32_t oldVersion;
        uint32_t newVersion;
        bool success;
        std::string error;
    };

    /**
     * @brief Callback for reload events
     */
    using ReloadCallback = std::function<void(const ReloadEvent&)>;

    /**
     * @brief Hot-reload configuration
     */
    struct HotReloadConfig
    {
        /// Enable hot-reload
        bool enabled = true;

        /// Delay before reloading (ms) - allows editors to finish saving
        uint32_t reloadDelayMs = 200;

        /// Batch reload interval (ms) - group multiple changes
        uint32_t batchIntervalMs = 100;

        /// Watch directories recursively
        bool watchRecursive = true;

        /// File extensions to watch (empty = all resource types)
        std::vector<std::string> watchExtensions;

        /// Patterns to ignore
        std::vector<std::string> ignorePatterns = {
            "*.tmp", "*.bak", "*~", ".git/*", ".svn/*"
        };

        /// Enable background watching
        bool backgroundWatch = true;

        /// Log reload events
        bool logReloads = true;
    };

    /**
     * @brief Hot-reload manager
     * 
     * Manages automatic reloading of resources when their source files change.
     * 
     * Usage:
     * @code
     * // In engine initialization
     * auto& hotReload = HotReloadManager::Get();
     * hotReload.Initialize(resourceManager, config);
     * hotReload.WatchDirectory("Assets/");
     * 
     * // Each frame
     * hotReload.Update();
     * 
     * // Optional: get notified of reloads
     * hotReload.OnReload([](const ReloadEvent& e) {
     *     if (e.success) {
     *         // Resource was reloaded
     *     }
     * });
     * @endcode
     */
    class HotReloadManager
    {
    public:
        HotReloadManager();
        ~HotReloadManager();

        // Non-copyable
        HotReloadManager(const HotReloadManager&) = delete;
        HotReloadManager& operator=(const HotReloadManager&) = delete;

        /// Get singleton instance
        static HotReloadManager& Get();

        // =====================================================================
        // Initialization
        // =====================================================================

        /**
         * @brief Initialize with resource manager
         * 
         * @param manager The resource manager to use for reloading
         * @param config Hot-reload configuration
         */
        void Initialize(ResourceManager* manager, 
                        const HotReloadConfig& config = HotReloadConfig());

        /**
         * @brief Shutdown hot-reload system
         */
        void Shutdown();

        /**
         * @brief Check if initialized
         */
        bool IsInitialized() const { return m_initialized; }

        /**
         * @brief Check if hot-reload is enabled
         */
        bool IsEnabled() const { return m_config.enabled; }

        /**
         * @brief Enable/disable hot-reload
         */
        void SetEnabled(bool enabled);

        // =====================================================================
        // Watch Management
        // =====================================================================

        /**
         * @brief Start watching a directory for changes
         * 
         * @param path Directory path
         * @param recursive Override recursive setting
         * @return Watch ID
         */
        uint32_t WatchDirectory(const std::string& path, bool recursive = true);

        /**
         * @brief Start watching a specific file
         * 
         * @param path File path
         * @return Watch ID
         */
        uint32_t WatchFile(const std::string& path);

        /**
         * @brief Stop watching
         */
        void StopWatching(uint32_t watchId);

        /**
         * @brief Stop all watches
         */
        void StopAllWatching();

        // =====================================================================
        // Resource Registration
        // =====================================================================

        /**
         * @brief Register a resource for hot-reload tracking
         * 
         * @param resource The resource to track
         * @param sourcePath Path to the source file
         */
        void RegisterResource(IResource* resource, const std::string& sourcePath);

        /**
         * @brief Unregister a resource
         */
        void UnregisterResource(ResourceId resourceId);

        /**
         * @brief Unregister a resource by path
         */
        void UnregisterResource(const std::string& path);

        // =====================================================================
        // Update
        // =====================================================================

        /**
         * @brief Process pending reloads
         * 
         * Call this once per frame to process file changes and reload resources.
         */
        void Update();

        /**
         * @brief Force reload a specific resource
         * 
         * @param resourceId Resource to reload
         * @return true if reload was successful
         */
        bool ForceReload(ResourceId resourceId);

        /**
         * @brief Force reload by path
         */
        bool ForceReload(const std::string& path);

        /**
         * @brief Force reload all registered resources
         */
        void ForceReloadAll();

        // =====================================================================
        // Callbacks
        // =====================================================================

        /**
         * @brief Register callback for reload events
         * 
         * @param callback Callback function
         * @return Callback ID for removal
         */
        uint32_t OnReload(ReloadCallback callback);

        /**
         * @brief Remove a reload callback
         */
        void RemoveReloadCallback(uint32_t callbackId);

        // =====================================================================
        // Query
        // =====================================================================

        /**
         * @brief Get resource version
         */
        uint32_t GetResourceVersion(ResourceId resourceId) const;

        /**
         * @brief Get resource version by path
         */
        uint32_t GetResourceVersion(const std::string& path) const;

        /**
         * @brief Get pending reload count
         */
        size_t GetPendingReloadCount() const;

        /**
         * @brief Get reload statistics
         */
        struct ReloadStats
        {
            size_t totalReloads = 0;
            size_t successfulReloads = 0;
            size_t failedReloads = 0;
            size_t watchedFiles = 0;
            size_t registeredResources = 0;
        };

        ReloadStats GetStats() const;

    private:
        // File change handler
        void OnFileChanged(const FileChangeEvent& event);

        // Reload processing
        void ProcessPendingReloads();
        bool ReloadResource(ResourceVersion& version);
        void NotifyReload(const ReloadEvent& event);

        // Resource mapping
        ResourceVersion* FindVersionByPath(const std::string& path);
        ResourceVersion* FindVersionById(ResourceId id);

        // Configuration
        bool m_initialized = false;
        HotReloadConfig m_config;
        ResourceManager* m_resourceManager = nullptr;

        // File watcher
        std::unique_ptr<FileWatcher> m_fileWatcher;

        // Resource tracking
        std::unordered_map<ResourceId, ResourceVersion> m_resourceVersions;
        std::unordered_map<std::string, ResourceId> m_pathToResourceId;
        mutable std::mutex m_resourceMutex;

        // Pending reloads
        std::vector<std::string> m_pendingReloads;
        mutable std::mutex m_pendingMutex;
        std::chrono::steady_clock::time_point m_lastBatchTime;

        // Callbacks
        std::unordered_map<uint32_t, ReloadCallback> m_reloadCallbacks;
        uint32_t m_nextCallbackId = 1;
        std::mutex m_callbackMutex;

        // Statistics
        std::atomic<size_t> m_totalReloads{0};
        std::atomic<size_t> m_successfulReloads{0};
        std::atomic<size_t> m_failedReloads{0};
    };

} // namespace RVX::Resource
