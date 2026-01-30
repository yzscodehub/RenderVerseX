#include "Resource/HotReloadManager.h"
#include "Resource/ResourceManager.h"
#include "Core/Log.h"

#include <algorithm>
#include <chrono>

namespace RVX::Resource
{
    // =========================================================================
    // Singleton
    // =========================================================================

    HotReloadManager& HotReloadManager::Get()
    {
        static HotReloadManager instance;
        return instance;
    }

    // =========================================================================
    // Construction
    // =========================================================================

    HotReloadManager::HotReloadManager()
    {
    }

    HotReloadManager::~HotReloadManager()
    {
        Shutdown();
    }

    // =========================================================================
    // Initialization
    // =========================================================================

    void HotReloadManager::Initialize(ResourceManager* manager,
                                       const HotReloadConfig& config)
    {
        if (m_initialized)
        {
            RVX_CORE_WARN("HotReloadManager: Already initialized");
            return;
        }

        m_resourceManager = manager;
        m_config = config;

        // Create file watcher
        m_fileWatcher = std::make_unique<FileWatcher>();

        if (config.backgroundWatch)
        {
            m_fileWatcher->StartBackground();
        }

        m_lastBatchTime = std::chrono::steady_clock::now();
        m_initialized = true;

        RVX_CORE_INFO("HotReloadManager: Initialized (enabled={})", config.enabled);
    }

    void HotReloadManager::Shutdown()
    {
        if (!m_initialized)
        {
            return;
        }

        // Stop all watching
        if (m_fileWatcher)
        {
            m_fileWatcher->StopBackground();
            m_fileWatcher->UnwatchAll();
            m_fileWatcher.reset();
        }

        // Clear all data
        {
            std::lock_guard<std::mutex> lock(m_resourceMutex);
            m_resourceVersions.clear();
            m_pathToResourceId.clear();
        }

        {
            std::lock_guard<std::mutex> lock(m_pendingMutex);
            m_pendingReloads.clear();
        }

        {
            std::lock_guard<std::mutex> lock(m_callbackMutex);
            m_reloadCallbacks.clear();
        }

        m_resourceManager = nullptr;
        m_initialized = false;

        RVX_CORE_INFO("HotReloadManager: Shutdown");
    }

    void HotReloadManager::SetEnabled(bool enabled)
    {
        m_config.enabled = enabled;

        if (m_config.logReloads)
        {
            RVX_CORE_INFO("HotReloadManager: {}", enabled ? "Enabled" : "Disabled");
        }
    }

    // =========================================================================
    // Watch Management
    // =========================================================================

    uint32_t HotReloadManager::WatchDirectory(const std::string& path, bool recursive)
    {
        if (!m_initialized || !m_fileWatcher)
        {
            return 0;
        }

        WatchOptions options;
        options.recursive = recursive;
        options.extensions = m_config.watchExtensions;
        options.ignorePatterns = m_config.ignorePatterns;
        options.debounceMs = m_config.reloadDelayMs;

        auto callback = [this](const FileChangeEvent& event) {
            OnFileChanged(event);
        };

        return m_fileWatcher->Watch(path, callback, options);
    }

    uint32_t HotReloadManager::WatchFile(const std::string& path)
    {
        if (!m_initialized || !m_fileWatcher)
        {
            return 0;
        }

        auto callback = [this](const FileChangeEvent& event) {
            OnFileChanged(event);
        };

        return m_fileWatcher->WatchFile(path, callback);
    }

    void HotReloadManager::StopWatching(uint32_t watchId)
    {
        if (m_fileWatcher)
        {
            m_fileWatcher->Unwatch(watchId);
        }
    }

    void HotReloadManager::StopAllWatching()
    {
        if (m_fileWatcher)
        {
            m_fileWatcher->UnwatchAll();
        }
    }

    // =========================================================================
    // Resource Registration
    // =========================================================================

    void HotReloadManager::RegisterResource(IResource* resource, const std::string& sourcePath)
    {
        if (!resource)
        {
            return;
        }

        std::filesystem::path absPath = std::filesystem::absolute(sourcePath);
        std::string absolutePath = absPath.string();

        std::lock_guard<std::mutex> lock(m_resourceMutex);

        ResourceVersion version;
        version.resourceId = resource->GetId();
        version.path = absolutePath;
        version.version = 1;
        version.pendingReload = false;

        if (std::filesystem::exists(absolutePath))
        {
            version.lastModified = std::filesystem::last_write_time(absolutePath);
        }

        m_resourceVersions[resource->GetId()] = version;
        m_pathToResourceId[absolutePath] = resource->GetId();

        if (m_config.logReloads)
        {
            RVX_CORE_DEBUG("HotReloadManager: Registered {} for hot-reload", 
                absPath.filename().string());
        }
    }

    void HotReloadManager::UnregisterResource(ResourceId resourceId)
    {
        std::lock_guard<std::mutex> lock(m_resourceMutex);

        auto it = m_resourceVersions.find(resourceId);
        if (it != m_resourceVersions.end())
        {
            m_pathToResourceId.erase(it->second.path);
            m_resourceVersions.erase(it);
        }
    }

    void HotReloadManager::UnregisterResource(const std::string& path)
    {
        std::filesystem::path absPath = std::filesystem::absolute(path);
        std::string absolutePath = absPath.string();

        std::lock_guard<std::mutex> lock(m_resourceMutex);

        auto it = m_pathToResourceId.find(absolutePath);
        if (it != m_pathToResourceId.end())
        {
            m_resourceVersions.erase(it->second);
            m_pathToResourceId.erase(it);
        }
    }

    // =========================================================================
    // Update
    // =========================================================================

    void HotReloadManager::Update()
    {
        if (!m_initialized || !m_config.enabled)
        {
            return;
        }

        // Update file watcher (if not using background thread)
        if (m_fileWatcher && !m_fileWatcher->IsBackgroundRunning())
        {
            m_fileWatcher->Update();
        }

        // Process pending reloads with batching
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - m_lastBatchTime).count();

        if (elapsed >= m_config.batchIntervalMs)
        {
            ProcessPendingReloads();
            m_lastBatchTime = now;
        }
    }

    bool HotReloadManager::ForceReload(ResourceId resourceId)
    {
        std::lock_guard<std::mutex> lock(m_resourceMutex);

        auto* version = FindVersionById(resourceId);
        if (!version)
        {
            return false;
        }

        return ReloadResource(*version);
    }

    bool HotReloadManager::ForceReload(const std::string& path)
    {
        std::filesystem::path absPath = std::filesystem::absolute(path);
        std::string absolutePath = absPath.string();

        std::lock_guard<std::mutex> lock(m_resourceMutex);

        auto* version = FindVersionByPath(absolutePath);
        if (!version)
        {
            return false;
        }

        return ReloadResource(*version);
    }

    void HotReloadManager::ForceReloadAll()
    {
        std::lock_guard<std::mutex> lock(m_resourceMutex);

        for (auto& [id, version] : m_resourceVersions)
        {
            ReloadResource(version);
        }
    }

    // =========================================================================
    // Callbacks
    // =========================================================================

    uint32_t HotReloadManager::OnReload(ReloadCallback callback)
    {
        std::lock_guard<std::mutex> lock(m_callbackMutex);
        uint32_t id = m_nextCallbackId++;
        m_reloadCallbacks[id] = std::move(callback);
        return id;
    }

    void HotReloadManager::RemoveReloadCallback(uint32_t callbackId)
    {
        std::lock_guard<std::mutex> lock(m_callbackMutex);
        m_reloadCallbacks.erase(callbackId);
    }

    // =========================================================================
    // Query
    // =========================================================================

    uint32_t HotReloadManager::GetResourceVersion(ResourceId resourceId) const
    {
        std::lock_guard<std::mutex> lock(m_resourceMutex);

        auto it = m_resourceVersions.find(resourceId);
        if (it != m_resourceVersions.end())
        {
            return it->second.version;
        }
        return 0;
    }

    uint32_t HotReloadManager::GetResourceVersion(const std::string& path) const
    {
        std::filesystem::path absPath = std::filesystem::absolute(path);
        std::string absolutePath = absPath.string();

        std::lock_guard<std::mutex> lock(m_resourceMutex);

        auto it = m_pathToResourceId.find(absolutePath);
        if (it != m_pathToResourceId.end())
        {
            auto vit = m_resourceVersions.find(it->second);
            if (vit != m_resourceVersions.end())
            {
                return vit->second.version;
            }
        }
        return 0;
    }

    size_t HotReloadManager::GetPendingReloadCount() const
    {
        std::lock_guard<std::mutex> lock(m_pendingMutex);
        return m_pendingReloads.size();
    }

    HotReloadManager::ReloadStats HotReloadManager::GetStats() const
    {
        ReloadStats stats;
        stats.totalReloads = m_totalReloads.load();
        stats.successfulReloads = m_successfulReloads.load();
        stats.failedReloads = m_failedReloads.load();

        {
            std::lock_guard<std::mutex> lock(m_resourceMutex);
            stats.registeredResources = m_resourceVersions.size();
        }

        if (m_fileWatcher)
        {
            stats.watchedFiles = m_fileWatcher->GetWatchCount();
        }

        return stats;
    }

    // =========================================================================
    // Internal Methods
    // =========================================================================

    void HotReloadManager::OnFileChanged(const FileChangeEvent& event)
    {
        if (!m_config.enabled)
        {
            return;
        }

        // Only handle modifications for now
        if (event.type != FileChangeType::Modified)
        {
            return;
        }

        // Check if this path is registered
        {
            std::lock_guard<std::mutex> lock(m_resourceMutex);
            if (m_pathToResourceId.find(event.path) == m_pathToResourceId.end())
            {
                return;
            }
        }

        // Queue for reload
        {
            std::lock_guard<std::mutex> lock(m_pendingMutex);
            
            // Avoid duplicates
            if (std::find(m_pendingReloads.begin(), m_pendingReloads.end(), event.path) 
                == m_pendingReloads.end())
            {
                m_pendingReloads.push_back(event.path);

                if (m_config.logReloads)
                {
                    std::filesystem::path fsPath(event.path);
                    RVX_CORE_INFO("HotReloadManager: File changed: {}", 
                        fsPath.filename().string());
                }
            }
        }
    }

    void HotReloadManager::ProcessPendingReloads()
    {
        std::vector<std::string> pathsToReload;
        {
            std::lock_guard<std::mutex> lock(m_pendingMutex);
            pathsToReload = std::move(m_pendingReloads);
            m_pendingReloads.clear();
        }

        if (pathsToReload.empty())
        {
            return;
        }

        std::lock_guard<std::mutex> lock(m_resourceMutex);

        for (const auto& path : pathsToReload)
        {
            auto* version = FindVersionByPath(path);
            if (version)
            {
                ReloadResource(*version);
            }
        }
    }

    bool HotReloadManager::ReloadResource(ResourceVersion& version)
    {
        if (!m_resourceManager)
        {
            return false;
        }

        m_totalReloads++;

        ReloadEvent event;
        event.resourceId = version.resourceId;
        event.path = version.path;
        event.oldVersion = version.version;

        // Get old resource
        event.oldResource = m_resourceManager->LoadResource(version.resourceId);

        // Try to reload
        // Note: This is a simplified approach. In production, you'd want to:
        // 1. Load the new resource without replacing the old one
        // 2. Swap internal data if successful
        // 3. Update GPU resources if needed

        try
        {
            // Unload the old resource first
            m_resourceManager->Unload(version.resourceId);

            // Load the new version
            event.newResource = m_resourceManager->LoadResource(version.path);

            if (event.newResource)
            {
                version.version++;
                version.lastModified = std::filesystem::last_write_time(version.path);
                
                event.newVersion = version.version;
                event.success = true;

                m_successfulReloads++;

                if (m_config.logReloads)
                {
                    std::filesystem::path fsPath(version.path);
                    RVX_CORE_INFO("HotReloadManager: Reloaded {} (v{})", 
                        fsPath.filename().string(), version.version);
                }
            }
            else
            {
                event.success = false;
                event.error = "Failed to load new version";
                m_failedReloads++;

                RVX_CORE_WARN("HotReloadManager: Failed to reload {}", version.path);
            }
        }
        catch (const std::exception& e)
        {
            event.success = false;
            event.error = e.what();
            m_failedReloads++;

            RVX_CORE_ERROR("HotReloadManager: Exception reloading {}: {}", 
                version.path, e.what());
        }

        // Notify callbacks
        NotifyReload(event);

        return event.success;
    }

    void HotReloadManager::NotifyReload(const ReloadEvent& event)
    {
        std::lock_guard<std::mutex> lock(m_callbackMutex);

        for (const auto& [id, callback] : m_reloadCallbacks)
        {
            if (callback)
            {
                callback(event);
            }
        }
    }

    ResourceVersion* HotReloadManager::FindVersionByPath(const std::string& path)
    {
        auto it = m_pathToResourceId.find(path);
        if (it != m_pathToResourceId.end())
        {
            auto vit = m_resourceVersions.find(it->second);
            if (vit != m_resourceVersions.end())
            {
                return &vit->second;
            }
        }
        return nullptr;
    }

    ResourceVersion* HotReloadManager::FindVersionById(ResourceId id)
    {
        auto it = m_resourceVersions.find(id);
        if (it != m_resourceVersions.end())
        {
            return &it->second;
        }
        return nullptr;
    }

} // namespace RVX::Resource
