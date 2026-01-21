#pragma once

/**
 * @file AssetHandle.h
 * @brief Smart handle for asset references
 */

#include "Asset/Asset.h"
#include <type_traits>

namespace RVX::Asset
{
    /**
     * @brief Smart handle for asset references
     * 
     * Provides:
     * - Reference counting (via RefCounted base)
     * - Type-safe access
     * - Loading state queries
     * - Async wait support
     */
    template<typename T>
    class AssetHandle
    {
        static_assert(std::is_base_of_v<Asset, T>, "T must derive from Asset");

    public:
        // =====================================================================
        // Construction
        // =====================================================================

        AssetHandle() = default;
        AssetHandle(std::nullptr_t) : m_asset(nullptr) {}

        explicit AssetHandle(T* asset) : m_asset(asset)
        {
            if (m_asset) m_asset->AddRef();
        }

        AssetHandle(const AssetHandle& other) : m_asset(other.m_asset)
        {
            if (m_asset) m_asset->AddRef();
        }

        AssetHandle(AssetHandle&& other) noexcept : m_asset(other.m_asset)
        {
            other.m_asset = nullptr;
        }

        template<typename U, typename = std::enable_if_t<std::is_base_of_v<T, U>>>
        AssetHandle(const AssetHandle<U>& other) : m_asset(other.Get())
        {
            if (m_asset) m_asset->AddRef();
        }

        ~AssetHandle()
        {
            Release();
        }

        // =====================================================================
        // Assignment
        // =====================================================================

        AssetHandle& operator=(const AssetHandle& other)
        {
            if (this != &other)
            {
                Release();
                m_asset = other.m_asset;
                if (m_asset) m_asset->AddRef();
            }
            return *this;
        }

        AssetHandle& operator=(AssetHandle&& other) noexcept
        {
            if (this != &other)
            {
                Release();
                m_asset = other.m_asset;
                other.m_asset = nullptr;
            }
            return *this;
        }

        AssetHandle& operator=(std::nullptr_t)
        {
            Release();
            return *this;
        }

        // =====================================================================
        // Access
        // =====================================================================

        T* Get() const { return m_asset; }
        T* operator->() const { return m_asset; }
        T& operator*() const { return *m_asset; }

        explicit operator bool() const { return m_asset != nullptr; }

        // =====================================================================
        // Comparison
        // =====================================================================

        bool operator==(const AssetHandle& other) const { return m_asset == other.m_asset; }
        bool operator!=(const AssetHandle& other) const { return m_asset != other.m_asset; }
        bool operator==(std::nullptr_t) const { return m_asset == nullptr; }
        bool operator!=(std::nullptr_t) const { return m_asset != nullptr; }

        // =====================================================================
        // State
        // =====================================================================

        bool IsValid() const { return m_asset != nullptr; }
        bool IsLoaded() const { return m_asset && m_asset->IsLoaded(); }
        bool IsLoading() const { return m_asset && m_asset->IsLoading(); }
        bool IsFailed() const { return m_asset && m_asset->IsFailed(); }

        AssetState GetState() const 
        { 
            return m_asset ? m_asset->GetState() : AssetState::Unloaded; 
        }

        AssetId GetId() const
        {
            return m_asset ? m_asset->GetId() : InvalidAssetId;
        }

        // =====================================================================
        // Async Wait
        // =====================================================================

        /// Wait for asset to finish loading (blocking)
        void WaitForLoad() const
        {
            // TODO: Implement proper async wait
            while (m_asset && m_asset->IsLoading())
            {
                // Busy wait for now
            }
        }

        /// Try to wait for load with timeout
        /// @return true if loaded, false if timeout
        bool TryWaitForLoad(uint32_t timeoutMs) const
        {
            (void)timeoutMs;
            // TODO: Implement with proper timeout
            WaitForLoad();
            return IsLoaded();
        }

        // =====================================================================
        // Reset
        // =====================================================================

        void Reset(T* ptr = nullptr)
        {
            Release();
            m_asset = ptr;
            if (m_asset) m_asset->AddRef();
        }

        T* Detach()
        {
            T* ptr = m_asset;
            m_asset = nullptr;
            return ptr;
        }

    private:
        void Release()
        {
            if (m_asset && m_asset->Release())
            {
                delete m_asset;
            }
            m_asset = nullptr;
        }

        T* m_asset = nullptr;

        template<typename U>
        friend class AssetHandle;
    };

    // Type aliases for common asset types
    class MeshAsset;
    class TextureAsset;
    class MaterialAsset;
    class ShaderAsset;
    class SkeletonAsset;
    class AnimationAsset;
    class ModelAsset;
    class SceneAsset;

    using MeshHandle = AssetHandle<MeshAsset>;
    using TextureHandle = AssetHandle<TextureAsset>;
    using MaterialHandle = AssetHandle<MaterialAsset>;
    using ShaderHandle = AssetHandle<ShaderAsset>;
    using SkeletonHandle = AssetHandle<SkeletonAsset>;
    using AnimationHandle = AssetHandle<AnimationAsset>;
    using ModelHandle = AssetHandle<ModelAsset>;
    using SceneHandle = AssetHandle<SceneAsset>;

} // namespace RVX::Asset
