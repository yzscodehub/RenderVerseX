#pragma once

/**
 * @file Asset.h
 * @brief Base asset class and common types
 */

#include "Core/RefCounted.h"
#include "Core/Types.h"
#include <string>
#include <vector>
#include <atomic>
#include <functional>

namespace RVX::Asset
{
    /// Asset identifier type (64-bit hash or GUID)
    using AssetId = uint64_t;
    static constexpr AssetId InvalidAssetId = 0;

    /**
     * @brief Asset loading state
     */
    enum class AssetState : uint8_t
    {
        Unloaded,   // Not loaded
        Loading,    // Currently loading
        Loaded,     // Successfully loaded
        Failed,     // Failed to load
        Unloading   // Being unloaded
    };

    /**
     * @brief Asset type enumeration
     */
    enum class AssetType : uint32_t
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
     * @brief Base class for all assets
     * 
     * Provides:
     * - Unique identification (AssetId)
     * - Loading state tracking
     * - Dependency tracking
     * - Memory usage reporting
     */
    class Asset : public RefCounted
    {
    public:
        Asset();
        ~Asset() override;

        // Non-copyable
        Asset(const Asset&) = delete;
        Asset& operator=(const Asset&) = delete;

        // =====================================================================
        // Identity
        // =====================================================================

        AssetId GetId() const { return m_id; }
        void SetId(AssetId id) { m_id = id; }

        const std::string& GetPath() const { return m_path; }
        void SetPath(const std::string& path) { m_path = path; }

        const std::string& GetName() const { return m_name; }
        void SetName(const std::string& name) { m_name = name; }

        // =====================================================================
        // Type
        // =====================================================================

        virtual AssetType GetType() const { return AssetType::Unknown; }
        virtual const char* GetTypeName() const { return "Unknown"; }

        // =====================================================================
        // State
        // =====================================================================

        AssetState GetState() const { return m_state.load(std::memory_order_acquire); }
        bool IsLoaded() const { return GetState() == AssetState::Loaded; }
        bool IsLoading() const { return GetState() == AssetState::Loading; }
        bool IsFailed() const { return GetState() == AssetState::Failed; }

        // =====================================================================
        // Dependencies
        // =====================================================================

        /// Get required dependencies (must be loaded before this asset)
        virtual std::vector<AssetId> GetRequiredDependencies() const { return {}; }

        /// Get optional dependencies (loaded if available)
        virtual std::vector<AssetId> GetOptionalDependencies() const { return {}; }

        /// Get all dependencies
        std::vector<AssetId> GetAllDependencies() const;

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

        using LoadCallback = std::function<void(Asset*)>;

        void SetOnLoaded(LoadCallback callback) { m_onLoaded = std::move(callback); }
        void SetOnUnloaded(LoadCallback callback) { m_onUnloaded = std::move(callback); }

    protected:
        void SetState(AssetState state);
        void NotifyLoaded();
        void NotifyUnloaded();

        friend class ResourceManager;
        friend class AssetLoader;

    private:
        AssetId m_id = InvalidAssetId;
        std::string m_path;
        std::string m_name;
        std::atomic<AssetState> m_state{AssetState::Unloaded};

        LoadCallback m_onLoaded;
        LoadCallback m_onUnloaded;
    };

    // =========================================================================
    // Helper: Generate AssetId from path
    // =========================================================================
    
    /// Generate asset ID from path (FNV-1a hash)
    AssetId GenerateAssetId(const std::string& path);

    /// Get type name for AssetType
    const char* GetAssetTypeName(AssetType type);

} // namespace RVX::Asset
