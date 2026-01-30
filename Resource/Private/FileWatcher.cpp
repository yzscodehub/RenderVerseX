#include "Resource/FileWatcher.h"
#include "Core/Log.h"

#include <algorithm>
#include <chrono>
#include <regex>

// Platform-specific includes
#ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN
    #include <windows.h>
#elif defined(__linux__)
    #include <sys/inotify.h>
    #include <unistd.h>
    #include <poll.h>
#elif defined(__APPLE__)
    #include <CoreServices/CoreServices.h>
#endif

namespace RVX::Resource
{
    // =========================================================================
    // Construction
    // =========================================================================

    FileWatcher::FileWatcher()
    {
    }

    FileWatcher::~FileWatcher()
    {
        StopBackground();
        UnwatchAll();
    }

    // =========================================================================
    // Watch Management
    // =========================================================================

    uint32_t FileWatcher::Watch(const std::string& path,
                                 FileChangeCallback callback,
                                 const WatchOptions& options)
    {
        std::lock_guard<std::mutex> lock(m_watchMutex);

        std::filesystem::path fsPath = std::filesystem::absolute(path);
        if (!std::filesystem::exists(fsPath))
        {
            RVX_CORE_WARN("FileWatcher: Path does not exist: {}", path);
            return 0;
        }

        WatchEntry entry;
        entry.id = m_nextWatchId++;
        entry.path = fsPath.string();
        entry.isDirectory = std::filesystem::is_directory(fsPath);
        entry.options = options;
        entry.callback = std::move(callback);

        // Initialize file timestamps for polling
        InitializeTimestamps(entry);

        // Try to set up native watch
        if (!SetupNativeWatch(entry))
        {
            RVX_CORE_INFO("FileWatcher: Using polling for: {}", path);
        }

        m_watches[entry.id] = std::move(entry);

        RVX_CORE_INFO("FileWatcher: Watching {} (id={})", path, m_watches[entry.id - 1].id);

        return entry.id;
    }

    uint32_t FileWatcher::WatchFile(const std::string& path, FileChangeCallback callback)
    {
        WatchOptions options;
        options.recursive = false;

        std::filesystem::path fsPath = std::filesystem::absolute(path);
        std::string ext = fsPath.extension().string();
        if (!ext.empty())
        {
            options.extensions.push_back(ext);
        }

        return Watch(fsPath.parent_path().string(), std::move(callback), options);
    }

    void FileWatcher::Unwatch(uint32_t watchId)
    {
        std::lock_guard<std::mutex> lock(m_watchMutex);

        auto it = m_watches.find(watchId);
        if (it != m_watches.end())
        {
            TeardownNativeWatch(it->second);
            RVX_CORE_INFO("FileWatcher: Stopped watching (id={})", watchId);
            m_watches.erase(it);
        }
    }

    void FileWatcher::UnwatchAll()
    {
        std::lock_guard<std::mutex> lock(m_watchMutex);

        for (auto& [id, entry] : m_watches)
        {
            TeardownNativeWatch(entry);
        }
        m_watches.clear();

        RVX_CORE_INFO("FileWatcher: Stopped all watches");
    }

    // =========================================================================
    // Update
    // =========================================================================

    void FileWatcher::Update()
    {
        // Process native events if available
        ProcessNativeEvents();

        // Poll for changes (fallback or supplement)
        PollChanges();

        // Process queued events
        std::vector<PendingEvent> events;
        {
            std::lock_guard<std::mutex> lock(m_eventMutex);
            events = std::move(m_pendingEvents);
            m_pendingEvents.clear();
        }

        // Invoke callbacks
        for (const auto& pending : events)
        {
            std::lock_guard<std::mutex> watchLock(m_watchMutex);
            auto it = m_watches.find(pending.watchId);
            if (it != m_watches.end() && it->second.callback)
            {
                it->second.callback(pending.event);
            }
        }
    }

    void FileWatcher::StartBackground()
    {
        if (m_backgroundRunning.load())
        {
            return;
        }

        m_stopRequested.store(false);
        m_backgroundRunning.store(true);

        m_backgroundThread = std::thread(&FileWatcher::BackgroundLoop, this);

        RVX_CORE_INFO("FileWatcher: Started background monitoring");
    }

    void FileWatcher::StopBackground()
    {
        if (!m_backgroundRunning.load())
        {
            return;
        }

        m_stopRequested.store(true);

        if (m_backgroundThread.joinable())
        {
            m_backgroundThread.join();
        }

        m_backgroundRunning.store(false);

        RVX_CORE_INFO("FileWatcher: Stopped background monitoring");
    }

    // =========================================================================
    // Query
    // =========================================================================

    size_t FileWatcher::GetWatchCount() const
    {
        std::lock_guard<std::mutex> lock(m_watchMutex);
        return m_watches.size();
    }

    bool FileWatcher::IsWatching(const std::string& path) const
    {
        std::lock_guard<std::mutex> lock(m_watchMutex);
        std::filesystem::path fsPath = std::filesystem::absolute(path);

        for (const auto& [id, entry] : m_watches)
        {
            if (entry.path == fsPath.string())
            {
                return true;
            }
        }
        return false;
    }

    // =========================================================================
    // Internal Methods
    // =========================================================================

    void FileWatcher::PollChanges()
    {
        std::lock_guard<std::mutex> lock(m_watchMutex);

        for (auto& [id, entry] : m_watches)
        {
            if (!entry.isDirectory)
            {
                // Single file watch
                if (std::filesystem::exists(entry.path))
                {
                    auto currentTime = std::filesystem::last_write_time(entry.path);
                    auto& lastTime = entry.fileTimestamps[entry.path];

                    if (currentTime != lastTime && lastTime != std::filesystem::file_time_type{})
                    {
                        if (PassesDebounce(entry.path, entry.options.debounceMs))
                        {
                            FileChangeEvent event;
                            event.path = entry.path;
                            event.type = FileChangeType::Modified;
                            event.timestamp = currentTime;
                            EnqueueEvent(id, event);
                        }
                    }
                    lastTime = currentTime;
                }
            }
            else
            {
                // Directory watch
                std::unordered_map<std::string, std::filesystem::file_time_type> currentFiles;

                try
                {
                    auto iterator = entry.options.recursive ?
                        std::filesystem::recursive_directory_iterator(entry.path) :
                        std::filesystem::recursive_directory_iterator(entry.path, 
                            std::filesystem::directory_options::skip_permission_denied);

                    for (auto it = std::filesystem::recursive_directory_iterator(entry.path);
                         it != std::filesystem::recursive_directory_iterator(); ++it)
                    {
                        if (!entry.options.recursive && it.depth() > 0)
                        {
                            it.disable_recursion_pending();
                            continue;
                        }

                        const auto& dirEntry = *it;
                        if (!dirEntry.is_regular_file())
                        {
                            continue;
                        }

                        std::string filePath = dirEntry.path().string();

                        if (ShouldIgnore(filePath, entry.options))
                        {
                            continue;
                        }

                        currentFiles[filePath] = dirEntry.last_write_time();
                    }
                }
                catch (const std::filesystem::filesystem_error& e)
                {
                    RVX_CORE_WARN("FileWatcher: Filesystem error: {}", e.what());
                    continue;
                }

                // Check for modified and created files
                for (const auto& [filePath, currentTime] : currentFiles)
                {
                    auto it = entry.fileTimestamps.find(filePath);
                    if (it == entry.fileTimestamps.end())
                    {
                        // New file
                        if (PassesDebounce(filePath, entry.options.debounceMs))
                        {
                            FileChangeEvent event;
                            event.path = filePath;
                            event.type = FileChangeType::Created;
                            event.timestamp = currentTime;
                            EnqueueEvent(id, event);
                        }
                    }
                    else if (it->second != currentTime)
                    {
                        // Modified file
                        if (PassesDebounce(filePath, entry.options.debounceMs))
                        {
                            FileChangeEvent event;
                            event.path = filePath;
                            event.type = FileChangeType::Modified;
                            event.timestamp = currentTime;
                            EnqueueEvent(id, event);
                        }
                    }
                }

                // Check for deleted files
                for (const auto& [filePath, oldTime] : entry.fileTimestamps)
                {
                    if (currentFiles.find(filePath) == currentFiles.end())
                    {
                        if (PassesDebounce(filePath, entry.options.debounceMs))
                        {
                            FileChangeEvent event;
                            event.path = filePath;
                            event.type = FileChangeType::Deleted;
                            event.timestamp = std::filesystem::file_time_type::clock::now();
                            EnqueueEvent(id, event);
                        }
                    }
                }

                // Update timestamps
                entry.fileTimestamps = std::move(currentFiles);
            }
        }
    }

    void FileWatcher::BackgroundLoop()
    {
        while (!m_stopRequested.load())
        {
            ProcessNativeEvents();
            PollChanges();
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    bool FileWatcher::ShouldIgnore(const std::string& path, const WatchOptions& options) const
    {
        std::filesystem::path fsPath(path);
        std::string filename = fsPath.filename().string();

        // Check hidden files
        if (options.ignoreHidden && !filename.empty() && filename[0] == '.')
        {
            return true;
        }

        // Check extension filter
        if (!options.extensions.empty())
        {
            std::string ext = fsPath.extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

            bool found = false;
            for (const auto& allowed : options.extensions)
            {
                std::string lowerAllowed = allowed;
                std::transform(lowerAllowed.begin(), lowerAllowed.end(), lowerAllowed.begin(), ::tolower);
                if (ext == lowerAllowed || ext == "." + lowerAllowed)
                {
                    found = true;
                    break;
                }
            }
            if (!found)
            {
                return true;
            }
        }

        // Check ignore patterns (simple glob-style)
        for (const auto& pattern : options.ignorePatterns)
        {
            // Convert glob to regex
            std::string regexPattern;
            for (char c : pattern)
            {
                switch (c)
                {
                    case '*': regexPattern += ".*"; break;
                    case '?': regexPattern += "."; break;
                    case '.': regexPattern += "\\."; break;
                    default: regexPattern += c; break;
                }
            }

            try
            {
                std::regex re(regexPattern, std::regex::icase);
                if (std::regex_match(filename, re))
                {
                    return true;
                }
            }
            catch (const std::regex_error&)
            {
                // Invalid pattern, skip
            }
        }

        return false;
    }

    void FileWatcher::EnqueueEvent(uint32_t watchId, const FileChangeEvent& event)
    {
        std::lock_guard<std::mutex> lock(m_eventMutex);
        m_pendingEvents.push_back({ watchId, event });
    }

    bool FileWatcher::PassesDebounce(const std::string& path, uint32_t debounceMs)
    {
        auto now = std::chrono::steady_clock::now();
        auto it = m_debounceMap.find(path);

        if (it != m_debounceMap.end())
        {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - it->second).count();
            if (elapsed < debounceMs)
            {
                return false;
            }
        }

        m_debounceMap[path] = now;
        return true;
    }

    void FileWatcher::InitializeTimestamps(WatchEntry& entry)
    {
        if (!entry.isDirectory)
        {
            if (std::filesystem::exists(entry.path))
            {
                entry.fileTimestamps[entry.path] = std::filesystem::last_write_time(entry.path);
            }
        }
        else
        {
            UpdateTimestamps(entry);
        }
    }

    void FileWatcher::UpdateTimestamps(WatchEntry& entry)
    {
        entry.fileTimestamps.clear();

        try
        {
            for (auto it = std::filesystem::recursive_directory_iterator(entry.path);
                 it != std::filesystem::recursive_directory_iterator(); ++it)
            {
                if (!entry.options.recursive && it.depth() > 0)
                {
                    it.disable_recursion_pending();
                    continue;
                }

                const auto& dirEntry = *it;
                if (!dirEntry.is_regular_file())
                {
                    continue;
                }

                std::string filePath = dirEntry.path().string();
                if (!ShouldIgnore(filePath, entry.options))
                {
                    entry.fileTimestamps[filePath] = dirEntry.last_write_time();
                }
            }
        }
        catch (const std::filesystem::filesystem_error& e)
        {
            RVX_CORE_WARN("FileWatcher: Error scanning directory: {}", e.what());
        }
    }

    // =========================================================================
    // Platform-Specific (Stubs - polling fallback)
    // =========================================================================

    bool FileWatcher::SetupNativeWatch(WatchEntry& entry)
    {
        // For now, use polling fallback on all platforms
        // TODO: Implement platform-specific watching
        return false;
    }

    void FileWatcher::TeardownNativeWatch(WatchEntry& entry)
    {
        // Cleanup native handle if set
        if (entry.nativeHandle)
        {
#ifdef _WIN32
            // CloseHandle(entry.nativeHandle);
#endif
            entry.nativeHandle = nullptr;
        }
    }

    void FileWatcher::ProcessNativeEvents()
    {
        // Process platform-specific events
        // TODO: Implement platform-specific event processing
    }

} // namespace RVX::Resource
