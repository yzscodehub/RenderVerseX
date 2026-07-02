#pragma once

/**
 * @file SceneAssetHandle.h
 * @brief Scene-facing typed asset handles without depending on Resource.
 */

#include "Core/AssetResource.h"
#include "Core/RefCounted.h"

#include <type_traits>
#include <utility>

namespace RVX::Resource
{
    class MaterialResource;
    class MeshResource;
    class ModelResource;
    class TextureResource;

    template<typename T>
    class ResourceHandle;
} // namespace RVX::Resource

namespace RVX
{
    template<typename T>
    class SceneAssetHandle
    {
    public:
        SceneAssetHandle() = default;
        SceneAssetHandle(std::nullptr_t) : m_resource(nullptr) {}

        SceneAssetHandle(T* resource)
        {
            Reset(resource);
        }

        template<typename Handle,
                 typename = std::enable_if_t<!std::is_same_v<std::decay_t<Handle>, SceneAssetHandle>>>
        SceneAssetHandle(const Handle& handle)
        {
            Reset(handle.Get());
        }

        T* Get() const { return m_resource; }
        T* operator->() const { return m_resource; }
        T& operator*() const { return *m_resource; }

        template<typename Interface>
        Interface* As() const
        {
            return dynamic_cast<Interface*>(m_retainedResource.Get());
        }

        explicit operator bool() const { return IsValid(); }

        bool IsValid() const { return m_resource != nullptr; }

        bool IsLoaded() const
        {
            const IAssetResource* asset = GetAssetResource();
            return asset && asset->IsAssetResourceLoaded();
        }

        uint64 GetId() const
        {
            const IAssetResource* asset = GetAssetResource();
            return asset ? asset->GetAssetResourceId() : 0;
        }

        void Reset(T* resource = nullptr)
        {
            m_resource = resource;
            m_retainedResource.Reset(resource ? static_cast<RefCounted*>(resource) : nullptr);
        }

    private:
        const IAssetResource* GetAssetResource() const
        {
            return dynamic_cast<const IAssetResource*>(m_retainedResource.Get());
        }

        T* m_resource = nullptr;
        Ref<RefCounted> m_retainedResource;
    };

    using SceneMaterialHandle = SceneAssetHandle<Resource::MaterialResource>;
    using SceneMeshHandle = SceneAssetHandle<Resource::MeshResource>;
    using SceneModelHandle = SceneAssetHandle<Resource::ModelResource>;
    using SceneTextureHandle = SceneAssetHandle<Resource::TextureResource>;

} // namespace RVX
