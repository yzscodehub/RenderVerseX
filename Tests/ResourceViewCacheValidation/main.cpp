#include "Core/Core.h"
#include "RHI/RHI.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <new>
#include <vector>

#include "Render/Graph/ResourceViewCache.h"

using namespace RVX;

namespace
{
    class LogEnvironment final : public ::testing::Environment
    {
    public:
        void SetUp() override
        {
            Log::Initialize();
        }

        void TearDown() override
        {
            Log::Shutdown();
        }
    };

    [[maybe_unused]] ::testing::Environment* const g_logEnvironment =
        ::testing::AddGlobalTestEnvironment(new LogEnvironment());

    class FakeTexture final : public RHITexture
    {
    public:
        explicit FakeTexture(const RHITextureDesc& desc,
                             bool* destroyed = nullptr)
            : m_desc(desc)
            , m_destroyed(destroyed)
        {
        }

        ~FakeTexture() override
        {
            if (m_destroyed)
            {
                *m_destroyed = true;
            }
        }

        uint32 GetWidth() const override { return m_desc.width; }
        uint32 GetHeight() const override { return m_desc.height; }
        uint32 GetDepth() const override { return m_desc.depth; }
        uint32 GetMipLevels() const override { return m_desc.mipLevels; }
        uint32 GetArraySize() const override { return m_desc.arraySize; }
        RHIFormat GetFormat() const override { return m_desc.format; }
        RHITextureUsage GetUsage() const override { return m_desc.usage; }
        RHITextureDimension GetDimension() const override { return m_desc.dimension; }
        RHISampleCount GetSampleCount() const override { return m_desc.sampleCount; }

    private:
        RHITextureDesc m_desc;
        bool* m_destroyed = nullptr;
    };

    class FakeTextureView final : public RHITextureView
    {
    public:
        FakeTextureView(RHITexture* texture, const RHITextureViewDesc& desc)
            : RHITextureView(RHITextureRef(texture))
            , m_format(desc.format == RHIFormat::Unknown && texture ? texture->GetFormat() : desc.format)
            , m_range(desc.subresourceRange)
        {
        }

        RHIFormat GetFormat() const override { return m_format; }
        const RHISubresourceRange& GetSubresourceRange() const override { return m_range; }

    private:
        RHIFormat m_format = RHIFormat::Unknown;
        RHISubresourceRange m_range;
    };

    class FakeDevice final : public IRHIDevice
    {
    public:
        RHIBufferRef CreateBuffer(const RHIBufferDesc&) override { return nullptr; }
        RHITextureRef CreateTexture(const RHITextureDesc&) override { return nullptr; }
        RHITextureViewRef CreateTextureView(RHITexture* texture, const RHITextureViewDesc& desc = {}) override
        {
            ++createdTextureViewCount;
            createdTextureViewDescs.push_back(desc);
            return RHITextureViewRef(new FakeTextureView(texture, desc));
        }
        RHISamplerRef CreateSampler(const RHISamplerDesc&) override { return nullptr; }
        RHIShaderRef CreateShader(const RHIShaderDesc&) override { return nullptr; }
        RHIHeapRef CreateHeap(const RHIHeapDesc&) override { return nullptr; }
        RHITextureRef CreatePlacedTexture(RHIHeap*, uint64, const RHITextureDesc&) override { return nullptr; }
        RHIBufferRef CreatePlacedBuffer(RHIHeap*, uint64, const RHIBufferDesc&) override { return nullptr; }
        MemoryRequirements GetTextureMemoryRequirements(const RHITextureDesc&) override { return {}; }
        MemoryRequirements GetBufferMemoryRequirements(const RHIBufferDesc&) override { return {}; }
        RHIDescriptorSetLayoutRef CreateDescriptorSetLayout(const RHIDescriptorSetLayoutDesc&) override { return nullptr; }
        RHIPipelineLayoutRef CreatePipelineLayout(const RHIPipelineLayoutDesc&) override { return nullptr; }
        RHIPipelineRef CreateGraphicsPipeline(const RHIGraphicsPipelineDesc&) override { return nullptr; }
        RHIPipelineRef CreateComputePipeline(const RHIComputePipelineDesc&) override { return nullptr; }
        RHIDescriptorSetRef CreateDescriptorSet(const RHIDescriptorSetDesc&) override { return nullptr; }
        RHIQueryPoolRef CreateQueryPool(const RHIQueryPoolDesc&) override { return nullptr; }
        RHICommandContextRef CreateCommandContext(RHICommandQueueType) override { return nullptr; }
        uint64 SubmitCommandContext(RHICommandContext*, RHIFence* = nullptr) override { return 0; }
        uint64 SubmitCommandContexts(std::span<RHICommandContext* const>, RHIFence* = nullptr) override { return 0; }
        RHISwapChainRef CreateSwapChain(const RHISwapChainDesc&) override { return nullptr; }
        RHIFenceRef CreateFence(uint64 = 0) override { return nullptr; }
        void WaitForFence(RHIFence*, uint64) override {}
        void WaitIdle() override {}
        void BeginFrame() override {}
        void EndFrame() override {}
        uint32 GetCurrentFrameIndex() const override { return 0; }
        RHIStagingBufferRef CreateStagingBuffer(const RHIStagingBufferDesc&) override { return nullptr; }
        RHIRingBufferRef CreateRingBuffer(const RHIRingBufferDesc&) override { return nullptr; }
        RHIMemoryStats GetMemoryStats() const override { return {}; }
        void BeginResourceGroup(const char*) override {}
        void EndResourceGroup() override {}
        const RHICapabilities& GetCapabilities() const override { return m_capabilities; }
        RHIBackendType GetBackendType() const override { return RHIBackendType::None; }

        uint32 createdTextureViewCount = 0;
        std::vector<RHITextureViewDesc> createdTextureViewDescs;

    private:
        RHICapabilities m_capabilities;
    };

    TEST(ResourceViewCacheValidation, TextureViewKeyUsesFullDescriptorIdentity)
    {
        FakeDevice device;
        ResourceViewCache cache;
        cache.Initialize(&device);
        const RHITextureDesc textureDesc = RHITextureDesc::Texture2D(
            4, 4, RHIFormat::RGBA8_UNORM);
        auto textureA = MakeRef<FakeTexture>(textureDesc);
        auto textureB = MakeRef<FakeTexture>(textureDesc);

        RHITextureViewDesc descA;
        descA.format = RHIFormat::RGBA8_UNORM;
        descA.dimension = RHITextureDimension::Texture2D;
        descA.subresourceRange = RHISubresourceRange::All();

        RHITextureViewDesc descMip = descA;
        descMip.subresourceRange = RHISubresourceRange::Mip(1);

        RHITextureViewDesc descLayer = descA;
        descLayer.subresourceRange = RHISubresourceRange::Layer(1);

        RHITextureViewDesc descRenamed = descA;
        descRenamed.debugName = "DebugNameDoesNotAffectGPUViewIdentity";

        RHITextureViewDesc descRenderTarget = descA;
        descRenderTarget.type = RHITextureViewType::RenderTarget;

        RHITextureView* viewA = cache.GetTextureView(textureA.Get(), descA);
        ASSERT_NE(viewA, nullptr);
        EXPECT_EQ(cache.GetTextureView(textureA.Get(), descA), viewA);
        EXPECT_EQ(cache.GetTextureView(textureA.Get(), descRenamed), viewA);
        EXPECT_NE(cache.GetTextureView(textureB.Get(), descA), viewA);
        EXPECT_NE(cache.GetTextureView(textureA.Get(), descMip), viewA);
        EXPECT_NE(cache.GetTextureView(textureA.Get(), descLayer), viewA);
        EXPECT_NE(cache.GetTextureView(textureA.Get(), descRenderTarget), viewA);
        EXPECT_EQ(device.createdTextureViewCount, 5u);
        EXPECT_EQ(cache.GetStats().textureViewCount, 5u);

        cache.Shutdown();
    }

    TEST(ResourceViewCacheValidation, TextureViewCacheSeparatesViewsByTypeEvenWhenDescriptorRangeMatches)
    {
        FakeDevice device;
        ResourceViewCache cache;
        cache.Initialize(&device);

        RHITextureDesc textureDesc = RHITextureDesc::DepthStencil(4, 4, RHIFormat::D32_FLOAT);
        auto texture = MakeRef<FakeTexture>(textureDesc);

        RHITextureViewDesc srvDesc;
        srvDesc.format = RHIFormat::D32_FLOAT;
        srvDesc.dimension = RHITextureDimension::Texture2D;
        srvDesc.subresourceRange = RHISubresourceRange::All();
        srvDesc.subresourceRange.aspect = RHITextureAspect::Depth;
        srvDesc.type = RHITextureViewType::ShaderResource;

        RHITextureViewDesc dsvDesc = srvDesc;
        dsvDesc.type = RHITextureViewType::DepthStencil;

        RHITextureView* srv = cache.GetTextureView(texture.Get(), srvDesc);
        RHITextureView* dsv = cache.GetTextureView(texture.Get(), dsvDesc);
        RHITextureView* srvHit = cache.GetTextureView(texture.Get(), srvDesc);
        RHITextureView* dsvHit = cache.GetTextureView(texture.Get(), dsvDesc);

        ASSERT_NE(nullptr, srv);
        ASSERT_NE(nullptr, dsv);
        EXPECT_NE(srv, dsv);
        EXPECT_EQ(srv, srvHit);
        EXPECT_EQ(dsv, dsvHit);
        EXPECT_EQ(2u, device.createdTextureViewCount);
        EXPECT_EQ(2u, cache.GetStats().textureViewCount);

        cache.Shutdown();
    }

    TEST(ResourceViewCacheValidation, DefaultDepthSRVAndDSVDoNotAlias)
    {
        FakeDevice device;
        ResourceViewCache cache;
        cache.Initialize(&device);

        RHITextureDesc textureDesc = RHITextureDesc::DepthStencil(4, 4, RHIFormat::D32_FLOAT);
        auto texture = MakeRef<FakeTexture>(textureDesc);

        RHITextureView* dsv = cache.GetDefaultDSV(texture.Get());
        RHITextureView* srv = cache.GetDefaultSRV(texture.Get());
        RHITextureView* dsvHit = cache.GetDefaultDSV(texture.Get());
        RHITextureView* srvHit = cache.GetDefaultSRV(texture.Get());

        ASSERT_NE(nullptr, dsv);
        ASSERT_NE(nullptr, srv);
        EXPECT_NE(dsv, srv);
        EXPECT_EQ(dsv, dsvHit);
        EXPECT_EQ(srv, srvHit);
        ASSERT_EQ(2u, device.createdTextureViewDescs.size());
        EXPECT_EQ(RHITextureViewType::DepthStencil, device.createdTextureViewDescs[0].type);
        EXPECT_EQ(RHITextureAspect::Depth, device.createdTextureViewDescs[0].subresourceRange.aspect);
        EXPECT_EQ(RHITextureViewType::ShaderResource, device.createdTextureViewDescs[1].type);
        EXPECT_EQ(RHITextureAspect::Depth, device.createdTextureViewDescs[1].subresourceRange.aspect);
        EXPECT_EQ(2u, cache.GetStats().textureViewCount);

        cache.Shutdown();
    }

    TEST(ResourceViewCacheValidation, TextureViewCacheInvalidationUpdatesGeneration)
    {
        FakeDevice device;
        ResourceViewCache cache;
        cache.Initialize(&device);

        RHITextureDesc textureDesc = RHITextureDesc::Texture2D(4, 4, RHIFormat::RGBA8_UNORM);
        auto textureA = MakeRef<FakeTexture>(textureDesc);
        auto textureB = MakeRef<FakeTexture>(textureDesc);

        RHITextureViewDesc viewDesc;
        viewDesc.format = RHIFormat::RGBA8_UNORM;
        viewDesc.dimension = RHITextureDimension::Texture2D;
        viewDesc.subresourceRange = RHISubresourceRange::All();

        RHITextureView* viewA = cache.GetTextureView(textureA.Get(), viewDesc);
        RHITextureView* viewAHit = cache.GetTextureView(textureA.Get(), viewDesc);

        ASSERT_NE(nullptr, viewA);
        EXPECT_EQ(viewA, viewAHit);
        EXPECT_EQ(1u, device.createdTextureViewCount);
        EXPECT_EQ(1u, cache.GetStats().textureViewCount);

        const uint64 initialGeneration = cache.GetGeneration();
        cache.InvalidateTexture(textureB.Get());
        EXPECT_EQ(initialGeneration, cache.GetGeneration());
        EXPECT_EQ(1u, cache.GetStats().textureViewCount);

        cache.InvalidateTexture(textureA.Get());
        EXPECT_EQ(initialGeneration + 1, cache.GetGeneration());
        EXPECT_EQ(0u, cache.GetStats().textureViewCount);

        RHITextureView* recreatedView = cache.GetTextureView(textureA.Get(), viewDesc);
        ASSERT_NE(nullptr, recreatedView);
        EXPECT_EQ(2u, device.createdTextureViewCount);

        const uint64 generationAfterRecreate = cache.GetGeneration();
        cache.Clear();
        EXPECT_EQ(generationAfterRecreate + 1, cache.GetGeneration());
        EXPECT_EQ(0u, cache.GetStats().textureViewCount);

        cache.Shutdown();
    }

    TEST(ResourceViewCacheValidation, TextureViewsRemainCachedUntilExplicitInvalidation)
    {
        FakeDevice device;
        ResourceViewCache cache;
        cache.Initialize(&device);

        RHITextureDesc textureDesc = RHITextureDesc::Texture2D(4, 4, RHIFormat::RGBA8_UNORM);
        auto texture = MakeRef<FakeTexture>(textureDesc);

        RHITextureViewDesc viewDesc;
        viewDesc.format = RHIFormat::RGBA8_UNORM;
        viewDesc.dimension = RHITextureDimension::Texture2D;
        viewDesc.subresourceRange = RHISubresourceRange::All();

        RHITextureView* view = cache.GetTextureView(texture.Get(), viewDesc);
        ASSERT_NE(nullptr, view);
        EXPECT_EQ(1u, device.createdTextureViewCount);
        EXPECT_EQ(1u, cache.GetStats().textureViewCount);

        const uint64 initialGeneration = cache.GetGeneration();
        for (uint32 i = 0; i < 16; ++i)
        {
            cache.BeginFrame();
        }

        EXPECT_EQ(initialGeneration, cache.GetGeneration());
        EXPECT_EQ(1u, cache.GetStats().textureViewCount);
        EXPECT_EQ(view, cache.GetTextureView(texture.Get(), viewDesc));
        EXPECT_EQ(1u, device.createdTextureViewCount);

        cache.InvalidateTexture(texture.Get());
        EXPECT_EQ(initialGeneration + 1, cache.GetGeneration());
        EXPECT_EQ(0u, cache.GetStats().textureViewCount);

        cache.Shutdown();
    }

    TEST(ResourceViewCacheValidation, TextureViewBaseStronglyOwnsSourceTexture)
    {
        const RHITextureDesc textureDesc = RHITextureDesc::Texture2D(
            4, 4, RHIFormat::RGBA8_UNORM);
        bool textureDestroyed = false;
        auto texture = MakeRef<FakeTexture>(textureDesc, &textureDestroyed);
        RHITexture* const textureAddress = texture.Get();

        RHITextureViewDesc viewDesc;
        viewDesc.format = textureDesc.format;
        auto view = MakeRef<FakeTextureView>(texture.Get(), viewDesc);

        EXPECT_EQ(view->GetTexture(), textureAddress);
        texture.Reset();
        EXPECT_FALSE(textureDestroyed);
        EXPECT_EQ(view->GetTexture(), textureAddress);

        view.Reset();
        EXPECT_TRUE(textureDestroyed);
    }

    TEST(ResourceViewCacheValidation,
         ReconstructedResourceAtSameAddressGetsNewInstanceIdentity)
    {
        const RHITextureDesc textureDesc = RHITextureDesc::Texture2D(
            4, 4, RHIFormat::RGBA8_UNORM);
        alignas(FakeTexture) std::byte storage[sizeof(FakeTexture)];

        auto* first = new (storage) FakeTexture(textureDesc);
        const RHIResourceInstanceId firstId = first->GetResourceInstanceId();
        first->~FakeTexture();

        auto* second = new (storage) FakeTexture(textureDesc);
        const RHIResourceInstanceId secondId = second->GetResourceInstanceId();

        EXPECT_EQ(static_cast<void*>(first), static_cast<void*>(second));
        EXPECT_TRUE(firstId.IsValid());
        EXPECT_TRUE(secondId.IsValid());
        EXPECT_NE(firstId.value, secondId.value);

        second->~FakeTexture();
    }
} // namespace
