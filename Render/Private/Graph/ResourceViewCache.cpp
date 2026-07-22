/** @file ResourceViewCache.cpp @brief Completion-safe resource-view caching */

#include "Render/Graph/ResourceViewCache.h"

#include "Core/Assert.h"
#include "Core/Log.h"
#include "Resources/RenderRetirementQueue.h"
#include "Resources/RenderSubmissionTracker.h"

#include <functional>
#include <unordered_map>

namespace RVX
{
    class ResourceViewCache::Impl
    {
    public:
        struct TextureViewKey
        {
            RHITexture* texture = nullptr;
            RHIFormat format = RHIFormat::Unknown;
            RHITextureDimension dimension = RHITextureDimension::Texture2D;
            RHISubresourceRange subresourceRange;
            RHITextureViewType type = RHITextureViewType::ShaderResource;

            bool operator==(const TextureViewKey& other) const
            {
                return texture == other.texture && format == other.format &&
                       dimension == other.dimension && type == other.type &&
                       subresourceRange.baseMipLevel ==
                           other.subresourceRange.baseMipLevel &&
                       subresourceRange.mipLevelCount ==
                           other.subresourceRange.mipLevelCount &&
                       subresourceRange.baseArrayLayer ==
                           other.subresourceRange.baseArrayLayer &&
                       subresourceRange.arrayLayerCount ==
                           other.subresourceRange.arrayLayerCount &&
                       subresourceRange.aspect ==
                           other.subresourceRange.aspect;
            }
        };

        struct TextureViewKeyHash
        {
            size_t operator()(const TextureViewKey& key) const
            {
                size_t hash = std::hash<RHITexture*>{}(key.texture);
                const auto hashCombine = [&hash](size_t value)
                {
                    hash ^= value + 0x9e3779b97f4a7c15ULL +
                            (hash << 6) + (hash >> 2);
                };
                hashCombine(std::hash<uint32>{}(static_cast<uint32>(key.format)));
                hashCombine(std::hash<uint32>{}(static_cast<uint32>(key.dimension)));
                hashCombine(std::hash<uint32>{}(static_cast<uint32>(key.type)));
                hashCombine(std::hash<uint32>{}(key.subresourceRange.baseMipLevel));
                hashCombine(std::hash<uint32>{}(key.subresourceRange.mipLevelCount));
                hashCombine(std::hash<uint32>{}(key.subresourceRange.baseArrayLayer));
                hashCombine(std::hash<uint32>{}(key.subresourceRange.arrayLayerCount));
                hashCombine(std::hash<uint32>{}(
                    static_cast<uint32>(key.subresourceRange.aspect)));
                return hash;
            }
        };

        struct CachedTextureView
        {
            RHITextureViewRef view;
            RHITexture* texture = nullptr;
            GPUCompletionToken lastUse;
            bool usedSinceSubmission = false;
        };

        static TextureViewKey MakeKey(RHITexture* texture,
                                      const RHITextureViewDesc& desc)
        {
            TextureViewKey key;
            key.texture = texture;
            key.format = desc.format;
            key.dimension = desc.dimension;
            key.subresourceRange = desc.subresourceRange;
            key.type = desc.type;
            return key;
        }

        void Retire(CachedTextureView& cached)
        {
            if (!cached.view)
            {
                return;
            }
            if (retirementQueue == nullptr)
            {
                cached.view.Reset();
                return;
            }

            Ref<RefCounted> object(std::move(cached.view));
            RenderRetirementEntry entry{cached.lastUse, std::move(object), 0};
            RVX_ASSERT_MSG(retirementQueue->Enqueue(std::move(entry)),
                           "Resource view retirement transfer failed");
        }

        IRHIDevice* device = nullptr;
        RenderSubmissionTracker* submissionTracker = nullptr;
        RenderRetirementQueue* retirementQueue = nullptr;
        uint32 currentFrame = 0;
        uint64 generation = 0;
        std::unordered_map<TextureViewKey,
                           CachedTextureView,
                           TextureViewKeyHash> textureViews;
        Stats stats;
    };

    ResourceViewCache::ResourceViewCache()
        : m_impl(std::make_unique<Impl>())
    {
    }

    ResourceViewCache::~ResourceViewCache()
    {
        Shutdown();
    }

    void ResourceViewCache::Initialize(
        IRHIDevice* device,
        RenderSubmissionTracker* submissionTracker,
        RenderRetirementQueue* retirementQueue)
    {
        if (m_impl->device)
        {
            RVX_CORE_WARN("ResourceViewCache: Already initialized");
            return;
        }
        m_impl->device = device;
        m_impl->submissionTracker = submissionTracker;
        m_impl->retirementQueue = retirementQueue;
        m_impl->currentFrame = 0;
        m_impl->generation = 0;
        m_impl->stats = {};
        RVX_CORE_DEBUG("ResourceViewCache: Initialized");
    }

    void ResourceViewCache::Shutdown()
    {
        if (!m_impl->device)
        {
            return;
        }
        Clear();
        m_impl->device = nullptr;
        m_impl->submissionTracker = nullptr;
        m_impl->retirementQueue = nullptr;
        RVX_CORE_DEBUG("ResourceViewCache: Shutdown");
    }

    bool ResourceViewCache::IsInitialized() const
    {
        return m_impl->device != nullptr;
    }

    uint64 ResourceViewCache::GetGeneration() const
    {
        return m_impl->generation;
    }

    void ResourceViewCache::NotifySubmission(
        const GPUCompletionToken& completion)
    {
        GPUCompletionToken normalized;
        RVX_ASSERT_MSG(MergeGPUCompletionToken(normalized, completion),
                       "Resource view cache received invalid completion token");
        for (auto& [key, cached] : m_impl->textureViews)
        {
            static_cast<void>(key);
            if (cached.usedSinceSubmission)
            {
                cached.lastUse = normalized;
                cached.usedSinceSubmission = false;
            }
        }
    }

    RHITextureView* ResourceViewCache::GetTextureView(
        RHITexture* texture,
        const RHITextureViewDesc& desc)
    {
        if (!m_impl->device || !texture)
        {
            return nullptr;
        }

        const Impl::TextureViewKey key = Impl::MakeKey(texture, desc);
        const auto existing = m_impl->textureViews.find(key);
        if (existing != m_impl->textureViews.end())
        {
            existing->second.usedSinceSubmission = true;
            ++m_impl->stats.cacheHits;
            return existing->second.view.Get();
        }

        RHITextureViewRef view = m_impl->device->CreateTextureView(texture, desc);
        if (!view)
        {
            RVX_CORE_ERROR("ResourceViewCache: Failed to create texture view");
            return nullptr;
        }

        Impl::CachedTextureView cached;
        cached.view = std::move(view);
        cached.texture = texture;
        cached.usedSinceSubmission = true;
        RHITextureView* result = cached.view.Get();
        m_impl->textureViews.emplace(key, std::move(cached));
        ++m_impl->stats.cacheMisses;
        ++m_impl->stats.textureViewCount;
        return result;
    }

    RHITextureView* ResourceViewCache::GetDefaultSRV(RHITexture* texture)
    {
        if (!texture)
        {
            return nullptr;
        }
        RHITextureViewDesc desc;
        desc.format = texture->GetFormat();
        desc.dimension = texture->GetDimension();
        desc.subresourceRange = RHISubresourceRange::All();
        if (IsDepthFormat(desc.format))
        {
            desc.subresourceRange.aspect = RHITextureAspect::Depth;
        }
        desc.type = RHITextureViewType::ShaderResource;
        desc.debugName = "DefaultSRV";
        return GetTextureView(texture, desc);
    }

    RHITextureView* ResourceViewCache::GetDefaultRTV(RHITexture* texture)
    {
        if (!texture)
        {
            return nullptr;
        }
        RHITextureViewDesc desc;
        desc.format = texture->GetFormat();
        desc.dimension = texture->GetDimension();
        desc.subresourceRange = RHISubresourceRange::All();
        desc.type = RHITextureViewType::RenderTarget;
        desc.debugName = "DefaultRTV";
        return GetTextureView(texture, desc);
    }

    RHITextureView* ResourceViewCache::GetDefaultDSV(RHITexture* texture)
    {
        if (!texture)
        {
            return nullptr;
        }
        RHITextureViewDesc desc;
        desc.format = texture->GetFormat();
        desc.dimension = texture->GetDimension();
        desc.subresourceRange = RHISubresourceRange::All();
        desc.type = RHITextureViewType::DepthStencil;
        if (IsDepthFormat(desc.format))
        {
            desc.subresourceRange.aspect = RHITextureAspect::Depth;
        }
        desc.debugName = "DefaultDSV";
        return GetTextureView(texture, desc);
    }

    RHITextureView* ResourceViewCache::GetDefaultUAV(RHITexture* texture)
    {
        if (!texture)
        {
            return nullptr;
        }
        RHITextureViewDesc desc;
        desc.format = texture->GetFormat();
        desc.dimension = texture->GetDimension();
        desc.subresourceRange = RHISubresourceRange::Mip(0);
        desc.type = RHITextureViewType::UnorderedAccess;
        desc.debugName = "DefaultUAV";
        return GetTextureView(texture, desc);
    }

    void ResourceViewCache::BeginFrame()
    {
        ++m_impl->currentFrame;
        ResetFrameStats();
    }

    void ResourceViewCache::InvalidateTexture(RHITexture* texture)
    {
        if (!texture)
        {
            return;
        }

        bool invalidated = false;
        for (auto it = m_impl->textureViews.begin();
             it != m_impl->textureViews.end();)
        {
            if (it->second.texture == texture)
            {
                m_impl->Retire(it->second);
                it = m_impl->textureViews.erase(it);
                invalidated = true;
                if (m_impl->stats.textureViewCount > 0)
                {
                    --m_impl->stats.textureViewCount;
                }
            }
            else
            {
                ++it;
            }
        }
        if (invalidated)
        {
            ++m_impl->generation;
        }
    }

    void ResourceViewCache::Clear()
    {
        if (!m_impl->textureViews.empty())
        {
            for (auto& [key, cached] : m_impl->textureViews)
            {
                static_cast<void>(key);
                m_impl->Retire(cached);
            }
            m_impl->textureViews.clear();
            ++m_impl->generation;
        }
        m_impl->stats.textureViewCount = 0;
    }

    ResourceViewCache::Stats ResourceViewCache::GetStats() const
    {
        return m_impl->stats;
    }

    void ResourceViewCache::ResetFrameStats()
    {
        m_impl->stats.cacheHits = 0;
        m_impl->stats.cacheMisses = 0;
    }
} // namespace RVX
