#pragma once

/**
 * @file IResource.h
 * @brief Base resource class and common types
 */

#include "Core/RefCounted.h"
#include "Core/Types.h"
#include <string>
#include <vector>
#include <atomic>
#include <functional>

namespace RVX::Resource
{
    /// Resource identifier type (64-bit hash or GUID)
    using ResourceId = uint64_t;
    static constexpr ResourceId InvalidResourceId = 0;

    /**
     * @brief Resource loading state
     */
    enum class ResourceState : uint8_t
    {
        Unloaded,   // Not loaded
        Loading,    // Currently loading
        Loaded,     // Successfully loaded
        Failed,     // Failed to load
        Unloading   // Being unloaded
    };

    /**
     * @brief Resource type enumeration
     */
    enum class ResourceType : uint32_t
    {
        Unknown = 0,
        Mesh,
        Texture,
        Material,
        Shader,
        Skeleton,
        Animation,
        Audio,
        Scene,
        Model,
        Prefab,
        Script,
        // Extensible...
        Custom = 1000
    };

    /**
     * @brief Base class for all resources
     * 
     * Provides:
     * - Unique identification (ResourceId)
     * - Loading state tracking
     * - Dependency tracking
     * - Memory usage reporting
     */
    class IResource : public RefCounted
    {
    public:
        IResource();
        ~IResource() override;

        // Non-copyable
        IResource(const IResource&) = delete;
        IResource& operator=(const IResource&) = delete;

        // =====================================================================
        // Identity
        // =====================================================================

        ResourceId GetId() const { return m_id; }
        void SetId(ResourceId id) { m_id = id; }

        const std::string& GetPath() const { return m_path; }
        void SetPath(const std::string& path) { m_path = path; }

        const std::string& GetName() const { return m_name; }
        void SetName(const std::string& name) { m_name = name; }

        // =====================================================================
        // Type
        // =====================================================================

        virtual ResourceType GetType() const { return ResourceType::Unknown; }
        virtual const char* GetTypeName() const { return "Unknown"; }

        // =====================================================================
        // State
        // =====================================================================

        ResourceState GetState() const { return m_state.load(std::memory_order_acquire); }
        bool IsLoaded() const { return GetState() == ResourceState::Loaded; }
        bool IsLoading() const { return GetState() == ResourceState::Loading; }
        bool IsFailed() const { return GetState() == ResourceState::Failed; }

        // =====================================================================
        // Dependencies
        // =====================================================================

        /// Get required dependencies (must be loaded before this resource)
        virtual std::vector<ResourceId> GetRequiredDependencies() const { return {}; }

        /// Get optional dependencies (loaded if available)
        virtual std::vector<ResourceId> GetOptionalDependencies() const { return {}; }

        /// Get all dependencies
        std::vector<ResourceId> GetAllDependencies() const;

        // =====================================================================
        // Memory
        // =====================================================================

        /// Get CPU memory usage in bytes
        virtual size_t GetMemoryUsage() const { return sizeof(*this); }

        /// Get GPU memory usage in bytes
        virtual size_t GetGPUMemoryUsage() const { return 0; }

        /// Get total memory usage
        size_t GetTotalMemoryUsage() const { return GetMemoryUsage() + GetGPUMemoryUsage(); }

        // =====================================================================
        // Callbacks
        // =====================================================================

        using LoadCallback = std::function<void(IResource*)>;

        void SetOnLoaded(LoadCallback callback) { m_onLoaded = std::move(callback); }
        void SetOnUnloaded(LoadCallback callback) { m_onUnloaded = std::move(callback); }

    protected:
        void SetState(ResourceState state);
        void NotifyLoaded();
        void NotifyUnloaded();

        friend class ResourceManager;
        friend class ResourceLoader;
        friend class TextureLoader;
        friend class ModelLoader;
        friend class DefaultResources;

    private:
        ResourceId m_id = InvalidResourceId;
        std::string m_path;
        std::string m_name;
        std::atomic<ResourceState> m_state{ResourceState::Unloaded};

        LoadCallback m_onLoaded;
        LoadCallback m_onUnloaded;
    };

    // =========================================================================
    // Helper: Generate ResourceId from path
    // =========================================================================
    
    /// Generate resource ID from path (FNV-1a hash)
    ResourceId GenerateResourceId(const std::string& path);

    /// Get type name for ResourceType
    const char* GetResourceTypeName(ResourceType type);

} // namespace RVX::Resource

// Backward compatibility - expose in RVX namespace
namespace RVX
{
    using Resource::ResourceId;
    using Resource::InvalidResourceId;
    using Resource::ResourceState;
    using Resource::ResourceType;
    using Resource::IResource;
    using Resource::GenerateResourceId;
    using Resource::GetResourceTypeName;
}
