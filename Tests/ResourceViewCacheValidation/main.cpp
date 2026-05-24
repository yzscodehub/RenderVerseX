#include "Core/Core.h"
#include "RHI/RHI.h"

#include <gtest/gtest.h>

#include <unordered_map>

#define private public
#include "Render/Graph/ResourceViewCache.h"
#undef private

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
        explicit FakeTexture(const RHITextureDesc& desc)
            : m_desc(desc)
        {
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
    };

    class FakeTextureView final : public RHITextureView
    {
    public:
        FakeTextureView(RHITexture* texture, const RHITextureViewDesc& desc)
            : m_texture(texture)
            , m_format(desc.format == RHIFormat::Unknown && texture ? texture->GetFormat() : desc.format)
            , m_range(desc.subresourceRange)
        {
        }

        RHITexture* GetTexture() const override { return m_texture; }
        RHIFormat GetFormat() const override { return m_format; }
        const RHISubresourceRange& GetSubresourceRange() const override { return m_range; }

    private:
        RHITexture* m_texture = nullptr;
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
        void SubmitCommandContext(RHICommandContext*, RHIFence* = nullptr) override {}
        void SubmitCommandContexts(std::span<RHICommandContext* const>, RHIFence* = nullptr) override {}
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

    private:
        RHICapabilities m_capabilities;
    };

    TEST(ResourceViewCacheValidation, TextureViewKeyUsesFullDescriptorIdentity)
    {
        auto* textureA = reinterpret_cast<RHITexture*>(0x1000);
        auto* textureB = reinterpret_cast<RHITexture*>(0x2000);
        auto makeKey = [](RHITexture* texture, const RHITextureViewDesc& desc)
        {
            ResourceViewCache::TextureViewKey key;
            key.texture = texture;
            key.format = desc.format;
            key.dimension = desc.dimension;
            key.subresourceRange = desc.subresourceRange;
            return key;
        };

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

        const auto keyA = makeKey(textureA, descA);
        const auto keyA2 = makeKey(textureA, descA);
        const auto keyRenamed = makeKey(textureA, descRenamed);
        const auto keyTextureB = makeKey(textureB, descA);
        const auto keyMip = makeKey(textureA, descMip);
        const auto keyLayer = makeKey(textureA, descLayer);

        EXPECT_TRUE(keyA == keyA2);
        EXPECT_TRUE(keyA == keyRenamed);
        EXPECT_FALSE(keyA == keyTextureB);
        EXPECT_FALSE(keyA == keyMip);
        EXPECT_FALSE(keyA == keyLayer);

        std::unordered_map<ResourceViewCache::TextureViewKey, int, ResourceViewCache::TextureViewKeyHash> keys;
        keys[keyA] = 1;
        keys[keyMip] = 2;
        keys[keyLayer] = 3;
        keys[keyTextureB] = 4;

        EXPECT_EQ(static_cast<size_t>(4), keys.size());
        EXPECT_EQ(1, keys[keyA2]);
    }

    TEST(ResourceViewCacheValidation, TextureViewCacheInvalidationUpdatesGeneration)
    {
        FakeDevice device;
        ResourceViewCache cache;
        cache.Initialize(&device);

        RHITextureDesc textureDesc = RHITextureDesc::Texture2D(4, 4, RHIFormat::RGBA8_UNORM);
        FakeTexture textureA(textureDesc);
        FakeTexture textureB(textureDesc);

        RHITextureViewDesc viewDesc;
        viewDesc.format = RHIFormat::RGBA8_UNORM;
        viewDesc.dimension = RHITextureDimension::Texture2D;
        viewDesc.subresourceRange = RHISubresourceRange::All();

        RHITextureView* viewA = cache.GetTextureView(&textureA, viewDesc);
        RHITextureView* viewAHit = cache.GetTextureView(&textureA, viewDesc);

        ASSERT_NE(nullptr, viewA);
        EXPECT_EQ(viewA, viewAHit);
        EXPECT_EQ(1u, device.createdTextureViewCount);
        EXPECT_EQ(1u, cache.GetStats().textureViewCount);

        const uint64 initialGeneration = cache.GetGeneration();
        cache.InvalidateTexture(&textureB);
        EXPECT_EQ(initialGeneration, cache.GetGeneration());
        EXPECT_EQ(1u, cache.GetStats().textureViewCount);

        cache.InvalidateTexture(&textureA);
        EXPECT_EQ(initialGeneration + 1, cache.GetGeneration());
        EXPECT_EQ(0u, cache.GetStats().textureViewCount);

        RHITextureView* recreatedView = cache.GetTextureView(&textureA, viewDesc);
        ASSERT_NE(nullptr, recreatedView);
        EXPECT_EQ(2u, device.createdTextureViewCount);

        const uint64 generationAfterRecreate = cache.GetGeneration();
        cache.Clear();
        EXPECT_EQ(generationAfterRecreate + 1, cache.GetGeneration());
        EXPECT_EQ(0u, cache.GetStats().textureViewCount);

        cache.Shutdown();
    }
} // namespace
