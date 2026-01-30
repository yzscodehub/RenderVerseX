#pragma once

/**
 * @file FileWatcher.h
 * @brief File system watcher for hot-reload support
 * 
 * Monitors files and directories for changes and notifies callbacks.
 * Supports:
 * - File modification detection
 * - File creation/deletion detection
 * - Directory recursive watching
 * - Debouncing for rapid changes
 */

#include "Core/Types.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <functional>
#include <memory>
#include <mutex>
#include <atomic>
#include <thread>
#include <filesystem>

namespace RVX::Resource
{
    /**
     * @brief Type of file change detected
     */
    enum class FileChangeType : uint8_t
    {
        Modified,   // File content changed
        Created,    // File was created
        Deleted,    // File was deleted
        Renamed     // File was renamed
    };

    /**
     * @brief Information about a file change
     */
    struct FileChangeEvent
    {
        std::string path;           // Absolute path to the file
        std::string oldPath;        // Old path (for rename events)
        FileChangeType type;        // Type of change
        std::filesystem::file_time_type timestamp;  // When change was detected
    };

    /**
     * @brief Callback for file change notifications
     */
    using FileChangeCallback = std::function<void(const FileChangeEvent&)>;

    /**
     * @brief Watch options
     */
    struct WatchOptions
    {
        /// Watch subdirectories recursively
        bool recursive = true;

        /// File extensions to watch (empty = all files)
        std::vector<std::string> extensions;

        /// Debounce delay in milliseconds (ignore rapid changes)
        uint32_t debounceMs = 100;

        /// Ignore hidden files (starting with .)
        bool ignoreHidden = true;

        /// Ignore patterns (glob-style)
        std::vector<std::string> ignorePatterns;
    };

    /**
     * @brief Platform-independent file watcher
     * 
     * Watches files and directories for changes using platform-specific APIs:
     * - Windows: ReadDirectoryChangesW
     * - Linux: inotify
     * - macOS: FSEvents
     * 
     * Falls back to polling if native watching is not available.
     */
    class FileWatcher
    {
    public:
        FileWatcher();
        ~FileWatcher();

        // Non-copyable
        FileWatcher(const FileWatcher&) = delete;
        FileWatcher& operator=(const FileWatcher&) = delete;

        // =====================================================================
        // Watch Management
        // =====================================================================

        /**
         * @brief Start watching a directory
         * 
         * @param path Directory path to watch
         * @param callback Callback for change notifications
         * @param options Watch options
         * @return Watch ID for later removal
         */
        uint32_t Watch(const std::string& path,
                       FileChangeCallback callback,
                       const WatchOptions& options = WatchOptions());

        /**
         * @brief Start watching a single file
         * 
         * @param path File path to watch
         * @param callback Callback for change notifications
         * @return Watch ID for later removal
         */
        uint32_t WatchFile(const std::string& path,
                           FileChangeCallback callback);

        /**
         * @brief Stop watching a specific watch
         * 
         * @param watchId Watch ID returned from Watch()
         */
        void Unwatch(uint32_t watchId);

        /**
         * @brief Stop all watches
         */
        void UnwatchAll();

        // =====================================================================
        // Update
        // =====================================================================

        /**
         * @brief Poll for changes
         * 
         * Call this periodically (e.g., once per frame) to process pending
         * file changes and invoke callbacks.
         */
        void Update();

        /**
         * @brief Start background monitoring thread
         * 
         * When enabled, changes are detected in a background thread
         * and queued for processing in Update().
         */
        void StartBackground();

        /**
         * @brief Stop background monitoring thread
         */
        void StopBackground();

        /**
         * @brief Check if background monitoring is running
         */
        bool IsBackgroundRunning() const { return m_backgroundRunning.load(); }

        // =====================================================================
        // Query
        // =====================================================================

        /**
         * @brief Get number of active watches
         */
        size_t GetWatchCount() const;

        /**
         * @brief Check if a path is being watched
         */
        bool IsWatching(const std::string& path) const;

    private:
        struct WatchEntry
        {
            uint32_t id;
            std::string path;
            bool isDirectory;
            WatchOptions options;
            FileChangeCallback callback;

            // For polling fallback
            std::unordered_map<std::string, std::filesystem::file_time_type> fileTimestamps;

            // Platform-specific handle
            void* nativeHandle = nullptr;
        };

        struct PendingEvent
        {
            uint32_t watchId;
            FileChangeEvent event;
        };

        // Watch management
        std::unordered_map<uint32_t, WatchEntry> m_watches;
        uint32_t m_nextWatchId = 1;
        mutable std::mutex m_watchMutex;

        // Event queue
        std::vector<PendingEvent> m_pendingEvents;
        std::mutex m_eventMutex;

        // Debouncing
        std::unordered_map<std::string, std::chrono::steady_clock::time_point> m_debounceMap;

        // Background thread
        std::atomic<bool> m_backgroundRunning{false};
        std::thread m_backgroundThread;
        std::atomic<bool> m_stopRequested{false};

        // Internal methods
        void PollChanges();
        void BackgroundLoop();
        bool ShouldIgnore(const std::string& path, const WatchOptions& options) const;
        void EnqueueEvent(uint32_t watchId, const FileChangeEvent& event);
        bool PassesDebounce(const std::string& path, uint32_t debounceMs);
        void InitializeTimestamps(WatchEntry& entry);
        void UpdateTimestamps(WatchEntry& entry);

        // Platform-specific
        bool SetupNativeWatch(WatchEntry& entry);
        void TeardownNativeWatch(WatchEntry& entry);
        void ProcessNativeEvents();
    };

} // namespace RVX::Resource
