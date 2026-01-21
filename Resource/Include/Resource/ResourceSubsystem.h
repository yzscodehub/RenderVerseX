#pragma once

/**
 * @file ResourceSubsystem.h
 * @brief Engine subsystem for resource management
 */

#include "Core/Subsystem/EngineSubsystem.h"
#include "Resource/ResourceManager.h"
#include "Resource/ResourceHandle.h"
#include <string>
#include <future>
#include <functional>

namespace RVX::Resource
{
    /**
     * @brief Engine subsystem for resource management
     * 
     * Provides:
     * - Subsystem lifecycle management
     * - Convenient access to ResourceManager
     * - Tick-based async processing
     */
    class ResourceSubsystem : public EngineSubsystem
    {
    public:
        ResourceSubsystem() = default;
        ~ResourceSubsystem() override = default;

        // =====================================================================
        // Subsystem Interface
        // =====================================================================

        const char* GetName() const override { return "ResourceSubsystem"; }

        void Initialize() override
        {
            ResourceManager::Get().Initialize(m_config);
        }

        void Initialize(const ResourceManagerConfig& config)
        {
            m_config = config;
            Initialize();
        }

        void Deinitialize() override
        {
            ResourceManager::Get().Shutdown();
        }

        bool ShouldTick() const override { return true; }

        void Tick(float deltaTime) override
        {
            (void)deltaTime;
            ResourceManager::Get().ProcessCompletedLoads();
            
            if (m_config.enableHotReload)
            {
                ResourceManager::Get().CheckForChanges();
            }
        }

        // =====================================================================
        // Resource Loading
        // =====================================================================

        /// Load resource by path
        template<typename T>
        ResourceHandle<T> Load(const std::string& path)
        {
            return ResourceManager::Get().Load<T>(path);
        }

        /// Load resource by ID
        template<typename T>
        ResourceHandle<T> Load(ResourceId id)
        {
            return ResourceManager::Get().Load<T>(id);
        }

        /// Load resource asynchronously
        template<typename T>
        std::future<ResourceHandle<T>> LoadAsync(const std::string& path)
        {
            return ResourceManager::Get().LoadAsync<T>(path);
        }

        /// Load with callback
        template<typename T>
        void LoadAsync(const std::string& path, std::function<void(ResourceHandle<T>)> callback)
        {
            ResourceManager::Get().LoadAsync<T>(path, std::move(callback));
        }

        // =====================================================================
        // Resource Query
        // =====================================================================

        /// Check if resource is loaded
        bool IsLoaded(const std::string& path) const
        {
            return ResourceManager::Get().IsLoaded(path);
        }

        bool IsLoaded(ResourceId id) const
        {
            return ResourceManager::Get().IsLoaded(id);
        }

        // =====================================================================
        // Unloading
        // =====================================================================

        /// Unload a resource
        void Unload(const std::string& path)
        {
            ResourceManager::Get().Unload(path);
        }

        void Unload(ResourceId id)
        {
            ResourceManager::Get().Unload(id);
        }

        /// Unload unused resources
        void UnloadUnused()
        {
            ResourceManager::Get().UnloadUnused();
        }

        // =====================================================================
        // Loader Registration
        // =====================================================================

        /// Register a resource loader
        void RegisterLoader(ResourceType type, std::unique_ptr<IResourceLoader> loader)
        {
            ResourceManager::Get().RegisterLoader(type, std::move(loader));
        }

        // =====================================================================
        // Hot Reload
        // =====================================================================

        /// Enable/disable hot reload
        void EnableHotReload(bool enable)
        {
            m_config.enableHotReload = enable;
            ResourceManager::Get().EnableHotReload(enable);
        }

        /// Register callback for resource reloads
        void OnResourceReloaded(std::function<void(ResourceId, IResource*)> callback)
        {
            ResourceManager::Get().OnResourceReloaded(std::move(callback));
        }

        // =====================================================================
        // Cache Control
        // =====================================================================

        /// Set cache memory limit
        void SetCacheLimit(size_t bytes)
        {
            ResourceManager::Get().SetCacheLimit(bytes);
        }

        /// Clear cache
        void ClearCache()
        {
            ResourceManager::Get().ClearCache();
        }

        // =====================================================================
        // Statistics
        // =====================================================================

        /// Get resource statistics
        ResourceManager::Stats GetStats() const
        {
            return ResourceManager::Get().GetStats();
        }

        // =====================================================================
        // Direct Access
        // =====================================================================

        /// Get the underlying ResourceManager
        ResourceManager& GetManager() { return ResourceManager::Get(); }
        const ResourceManager& GetManager() const { return ResourceManager::Get(); }

    private:
        ResourceManagerConfig m_config;
    };

} // namespace RVX::Resource

namespace RVX
{
    using Resource::ResourceSubsystem;
}
