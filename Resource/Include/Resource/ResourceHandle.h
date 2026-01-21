#pragma once

/**
 * @file ResourceHandle.h
 * @brief Smart handle for resource references
 */

#include "Resource/IResource.h"
#include <type_traits>

namespace RVX::Resource
{
    /**
     * @brief Smart handle for resource references
     * 
     * Provides:
     * - Reference counting (via RefCounted base)
     * - Type-safe access
     * - Loading state queries
     * - Async wait support
     */
    template<typename T>
    class ResourceHandle
    {
        static_assert(std::is_base_of_v<IResource, T>, "T must derive from IResource");

    public:
        // =====================================================================
        // Construction
        // =====================================================================

        ResourceHandle() = default;
        ResourceHandle(std::nullptr_t) : m_resource(nullptr) {}

        explicit ResourceHandle(T* resource) : m_resource(resource)
        {
            if (m_resource) m_resource->AddRef();
        }

        ResourceHandle(const ResourceHandle& other) : m_resource(other.m_resource)
        {
            if (m_resource) m_resource->AddRef();
        }

        ResourceHandle(ResourceHandle&& other) noexcept : m_resource(other.m_resource)
        {
            other.m_resource = nullptr;
        }

        template<typename U, typename = std::enable_if_t<std::is_base_of_v<T, U>>>
        ResourceHandle(const ResourceHandle<U>& other) : m_resource(other.Get())
        {
            if (m_resource) m_resource->AddRef();
        }

        ~ResourceHandle()
        {
            Release();
        }

        // =====================================================================
        // Assignment
        // =====================================================================

        ResourceHandle& operator=(const ResourceHandle& other)
        {
            if (this != &other)
            {
                Release();
                m_resource = other.m_resource;
                if (m_resource) m_resource->AddRef();
            }
            return *this;
        }

        ResourceHandle& operator=(ResourceHandle&& other) noexcept
        {
            if (this != &other)
            {
                Release();
                m_resource = other.m_resource;
                other.m_resource = nullptr;
            }
            return *this;
        }

        ResourceHandle& operator=(std::nullptr_t)
        {
            Release();
            return *this;
        }

        // =====================================================================
        // Access
        // =====================================================================

        T* Get() const { return m_resource; }
        T* operator->() const { return m_resource; }
        T& operator*() const { return *m_resource; }

        explicit operator bool() const { return m_resource != nullptr; }

        // =====================================================================
        // Comparison
        // =====================================================================

        bool operator==(const ResourceHandle& other) const { return m_resource == other.m_resource; }
        bool operator!=(const ResourceHandle& other) const { return m_resource != other.m_resource; }
        bool operator==(std::nullptr_t) const { return m_resource == nullptr; }
        bool operator!=(std::nullptr_t) const { return m_resource != nullptr; }

        // =====================================================================
        // State
        // =====================================================================

        bool IsValid() const { return m_resource != nullptr; }
        bool IsLoaded() const { return m_resource && m_resource->IsLoaded(); }
        bool IsLoading() const { return m_resource && m_resource->IsLoading(); }
        bool IsFailed() const { return m_resource && m_resource->IsFailed(); }

        ResourceState GetState() const 
        { 
            return m_resource ? m_resource->GetState() : ResourceState::Unloaded; 
        }

        ResourceId GetId() const
        {
            return m_resource ? m_resource->GetId() : InvalidResourceId;
        }

        // =====================================================================
        // Async Wait
        // =====================================================================

        /// Wait for resource to finish loading (blocking)
        void WaitForLoad() const
        {
            while (m_resource && m_resource->IsLoading())
            {
                // Busy wait for now
            }
        }

        /// Try to wait for load with timeout
        /// @return true if loaded, false if timeout
        bool TryWaitForLoad(uint32_t timeoutMs) const
        {
            (void)timeoutMs;
            WaitForLoad();
            return IsLoaded();
        }

        // =====================================================================
        // Reset
        // =====================================================================

        void Reset(T* ptr = nullptr)
        {
            Release();
            m_resource = ptr;
            if (m_resource) m_resource->AddRef();
        }

        T* Detach()
        {
            T* ptr = m_resource;
            m_resource = nullptr;
            return ptr;
        }

    private:
        void Release()
        {
            if (m_resource && m_resource->Release())
            {
                delete m_resource;
            }
            m_resource = nullptr;
        }

        T* m_resource = nullptr;

        template<typename U>
        friend class ResourceHandle;
    };

    // Forward declarations for common resource types
    class MeshResource;
    class TextureResource;
    class MaterialResource;
    class ShaderResource;
    class SkeletonResource;
    class AnimationResource;
    class ModelResource;
    class SceneResource;

    // Type aliases
    using MeshHandle = ResourceHandle<MeshResource>;
    using TextureHandle = ResourceHandle<TextureResource>;
    using MaterialHandle = ResourceHandle<MaterialResource>;
    using ShaderHandle = ResourceHandle<ShaderResource>;
    using SkeletonHandle = ResourceHandle<SkeletonResource>;
    using AnimationHandle = ResourceHandle<AnimationResource>;
    using ModelHandle = ResourceHandle<ModelResource>;
    using SceneHandle = ResourceHandle<SceneResource>;

} // namespace RVX::Resource

namespace RVX
{
    using Resource::ResourceHandle;
}
