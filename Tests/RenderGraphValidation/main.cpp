#include "Core/Core.h"
#include "Render/Graph/RenderGraph.h"
#include "Render/Graph/TransientResourcePool.h"
#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace RVX;

namespace
{
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

    class FakeBuffer final : public RHIBuffer
    {
    public:
        explicit FakeBuffer(const RHIBufferDesc& desc)
            : m_desc(desc)
        {
        }

        uint64 GetSize() const override { return m_desc.size; }
        RHIBufferUsage GetUsage() const override { return m_desc.usage; }
        RHIMemoryType GetMemoryType() const override { return m_desc.memoryType; }
        uint32 GetStride() const override { return m_desc.stride; }
        void* Map() override { return nullptr; }
        void Unmap() override {}

    private:
        RHIBufferDesc m_desc;
    };

    class FakeCommandContext final : public RHICommandContext
    {
    public:
        void Begin() override {}
        void End() override {}
        void Reset() override {}
        void BeginEvent(const char*, uint32 = 0) override {}
        void EndEvent() override {}
        void SetMarker(const char*, uint32 = 0) override {}
        void BufferBarrier(const RHIBufferBarrier& barrier) override { bufferBarriers.push_back(barrier); }
        void TextureBarrier(const RHITextureBarrier& barrier) override { textureBarriers.push_back(barrier); }
        void Barriers(std::span<const RHIBufferBarrier> buffers, std::span<const RHITextureBarrier> textures) override
        {
            bufferBarriers.insert(bufferBarriers.end(), buffers.begin(), buffers.end());
            textureBarriers.insert(textureBarriers.end(), textures.begin(), textures.end());
        }
        void BeginBarrier(const RHIBufferBarrier&) override {}
        void BeginBarrier(const RHITextureBarrier&) override {}
        void EndBarrier(const RHIBufferBarrier&) override {}
        void EndBarrier(const RHITextureBarrier&) override {}
        void BeginRenderPass(const RHIRenderPassDesc&) override {}
        void EndRenderPass() override {}
        void SetPipeline(RHIPipeline*) override {}
        void SetVertexBuffer(uint32, RHIBuffer*, uint64 = 0) override {}
        void SetVertexBuffers(uint32, std::span<RHIBuffer* const>, std::span<const uint64> = {}) override {}
        void SetIndexBuffer(RHIBuffer*, RHIFormat, uint64 = 0) override {}
        void SetDescriptorSet(uint32, RHIDescriptorSet*, std::span<const uint32> = {}) override {}
        void SetPushConstants(const void*, uint32, uint32 = 0) override {}
        void SetViewport(const RHIViewport&) override {}
        void SetViewports(std::span<const RHIViewport>) override {}
        void SetScissor(const RHIRect&) override {}
        void SetScissors(std::span<const RHIRect>) override {}
        void Draw(uint32, uint32 = 1, uint32 = 0, uint32 = 0) override {}
        void DrawIndexed(uint32, uint32 = 1, uint32 = 0, int32 = 0, uint32 = 0) override {}
        void DrawIndirect(RHIBuffer*, uint64, uint32, uint32) override {}
        void DrawIndexedIndirect(RHIBuffer*, uint64, uint32, uint32) override {}
        void Dispatch(uint32, uint32, uint32) override {}
        void DispatchIndirect(RHIBuffer*, uint64) override {}
        void CopyBuffer(RHIBuffer*, RHIBuffer*, uint64, uint64, uint64) override {}
        void CopyTexture(RHITexture*, RHITexture*, const RHITextureCopyDesc& = {}) override {}
        void CopyBufferToTexture(RHIBuffer*, RHITexture*, const RHIBufferTextureCopyDesc&) override {}
        void CopyTextureToBuffer(RHITexture*, RHIBuffer*, const RHIBufferTextureCopyDesc&) override {}
        void BeginQuery(RHIQueryPool*, uint32) override {}
        void EndQuery(RHIQueryPool*, uint32) override {}
        void WriteTimestamp(RHIQueryPool*, uint32) override {}
        void ResolveQueries(RHIQueryPool*, uint32, uint32, RHIBuffer*, uint64) override {}
        void ResetQueries(RHIQueryPool*, uint32, uint32) override {}
        void SetStencilReference(uint32) override {}
        void SetBlendConstants(const float[4]) override {}
        void SetDepthBias(float, float, float = 0.0f) override {}
        void SetDepthBounds(float, float) override {}
        void SetStencilReferenceSeparate(uint32, uint32) override {}
        void SetLineWidth(float) override {}
        void SignalFence(RHIFence*, uint64) override {}
        void WaitFence(RHIFence*, uint64) override {}

        std::vector<RHIBufferBarrier> bufferBarriers;
        std::vector<RHITextureBarrier> textureBarriers;
    };

    class FakeFence final : public RHIFence
    {
    public:
        explicit FakeFence(uint64 initialValue = 0)
            : m_completedValue(initialValue)
        {
        }

        uint64 GetCompletedValue() const override { return m_completedValue; }
        void Signal(uint64 value) override { m_completedValue = value; }
        void SignalOnQueue(uint64 value, RHICommandQueueType) override { m_completedValue = value; }
        void Wait(uint64 value, uint64 = UINT64_MAX) override { m_completedValue = value; }

    private:
        uint64 m_completedValue = 0;
    };

    class FakeDevice final : public IRHIDevice
    {
    public:
        RHIBufferRef CreateBuffer(const RHIBufferDesc& desc) override
        {
            ++createBufferCount;
            return RHIBufferRef(new FakeBuffer(desc));
        }

        RHITextureRef CreateTexture(const RHITextureDesc& desc) override
        {
            ++createTextureCount;
            return RHITextureRef(new FakeTexture(desc));
        }

        RHITextureViewRef CreateTextureView(RHITexture*, const RHITextureViewDesc& = {}) override { return {}; }
        RHISamplerRef CreateSampler(const RHISamplerDesc&) override { return {}; }
        RHIShaderRef CreateShader(const RHIShaderDesc&) override { return {}; }
        RHIHeapRef CreateHeap(const RHIHeapDesc&) override { return {}; }
        RHITextureRef CreatePlacedTexture(RHIHeap*, uint64, const RHITextureDesc&) override { return {}; }
        RHIBufferRef CreatePlacedBuffer(RHIHeap*, uint64, const RHIBufferDesc&) override { return {}; }
        MemoryRequirements GetTextureMemoryRequirements(const RHITextureDesc&) override { return {}; }
        MemoryRequirements GetBufferMemoryRequirements(const RHIBufferDesc&) override { return {}; }
        RHIDescriptorSetLayoutRef CreateDescriptorSetLayout(const RHIDescriptorSetLayoutDesc&) override { return {}; }
        RHIPipelineLayoutRef CreatePipelineLayout(const RHIPipelineLayoutDesc&) override { return {}; }
        RHIPipelineRef CreateGraphicsPipeline(const RHIGraphicsPipelineDesc&) override { return {}; }
        RHIPipelineRef CreateComputePipeline(const RHIComputePipelineDesc&) override { return {}; }
        RHIDescriptorSetRef CreateDescriptorSet(const RHIDescriptorSetDesc&) override { return {}; }
        RHIQueryPoolRef CreateQueryPool(const RHIQueryPoolDesc&) override { return {}; }
        RHICommandContextRef CreateCommandContext(RHICommandQueueType) override { return {}; }
        uint64 SubmitCommandContext(RHICommandContext*, RHIFence*) override { return 0; }
        uint64 SubmitCommandContexts(std::span<RHICommandContext* const>, RHIFence*) override { return 0; }
        RHISwapChainRef CreateSwapChain(const RHISwapChainDesc&) override { return {}; }
        RHIFenceRef CreateFence(uint64 initialValue = 0) override { return RHIFenceRef(new FakeFence(initialValue)); }
        void WaitForFence(RHIFence*, uint64) override {}
        void WaitIdle() override {}
        void BeginFrame() override {}
        void EndFrame() override {}
        uint32 GetCurrentFrameIndex() const override { return 0; }
        RHIStagingBufferRef CreateStagingBuffer(const RHIStagingBufferDesc&) override { return {}; }
        RHIRingBufferRef CreateRingBuffer(const RHIRingBufferDesc&) override { return {}; }
        RHIMemoryStats GetMemoryStats() const override { return {}; }
        void BeginResourceGroup(const char*) override {}
        void EndResourceGroup() override {}
        const RHICapabilities& GetCapabilities() const override { return m_capabilities; }
        RHIBackendType GetBackendType() const override { return RHIBackendType::DX12; }

        uint32 createTextureCount = 0;
        uint32 createBufferCount = 0;

    private:
        RHICapabilities m_capabilities;
    };

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
} // namespace

// =============================================================================
// RenderGraph Validation Tests
// =============================================================================

TEST(RenderGraphValidation, GraphCreation)
{
    RenderGraph graph;
    // Just test construction/destruction
}

TEST(RenderGraphValidation, TextureResourceCreation)
{
    RenderGraph graph;

    RHITextureDesc texDesc = RHITextureDesc::RenderTarget(1920, 1080, RHIFormat::RGBA16_FLOAT);
    texDesc.debugName = "TestRenderTarget";
    auto texture = graph.CreateTexture(texDesc);

    // Handle should be valid
    EXPECT_TRUE(texture.IsValid());
}

TEST(RenderGraphValidation, TransientResourcePoolReusesTexturesAcrossClear)
{
    FakeDevice device;
    TransientResourcePool pool;
    pool.Initialize(&device);

    RenderGraph graph;
    graph.SetDevice(&device);
    graph.SetTransientResourcePool(&pool);

    RHITextureDesc texDesc =
        RHITextureDesc::RenderTarget(128, 64, RHIFormat::RGBA8_UNORM);
    texDesc.debugName = "RenderGraphValidation.PooledTexture";

    auto buildFrame = [&]() {
        struct PassData
        {
            RGTextureHandle output;
        };

        RGTextureHandle texture = graph.CreateTexture(texDesc);
        graph.AddPass<PassData>(
            "WritePooledTexture",
            RenderGraphPassType::Graphics,
            [texture](RenderGraphBuilder& builder, PassData& data) {
                data.output = builder.Write(texture);
            },
            [](const PassData&, RHICommandContext&) {});
        graph.SetExportState(texture, RHIResourceState::ShaderResource);
        graph.Compile();
        FakeCommandContext ctx;
        graph.Execute(ctx);
    };

    pool.BeginFrame();
    graph.Clear();
    buildFrame();
    pool.EndFrame();

    EXPECT_EQ(1u, device.createTextureCount);
    EXPECT_EQ(1u, pool.GetStats().textureMisses);
    EXPECT_EQ(0u, pool.GetStats().textureHits);

    pool.BeginFrame();
    graph.Clear();
    buildFrame();
    pool.EndFrame();

    EXPECT_EQ(1u, device.createTextureCount);
    EXPECT_EQ(1u, pool.GetStats().textureHits);
    EXPECT_EQ(0u, pool.GetStats().textureMisses);

    graph.Clear();
    pool.Shutdown();
}

TEST(RenderGraphValidation, BufferResourceCreation)
{
    RenderGraph graph;

    RHIBufferDesc bufDesc;
    bufDesc.size = 1024 * 1024;
    bufDesc.usage = RHIBufferUsage::Structured;
    bufDesc.debugName = "TestStructuredBuffer";
    auto buffer = graph.CreateBuffer(bufDesc);

    EXPECT_TRUE(buffer.IsValid());
}

TEST(RenderGraphValidation, MultipleResources)
{
    RenderGraph graph;

    // Create multiple textures
    std::vector<RGTextureHandle> textures;
    for (int i = 0; i < 10; ++i)
    {
        RHITextureDesc desc = RHITextureDesc::RenderTarget(512, 512, RHIFormat::RGBA8_UNORM);
        textures.push_back(graph.CreateTexture(desc));
        EXPECT_TRUE(textures.back().IsValid());
    }

    // Create multiple buffers
    std::vector<RGBufferHandle> buffers;
    for (int i = 0; i < 10; ++i)
    {
        RHIBufferDesc desc;
        desc.size = 4096;
        desc.usage = RHIBufferUsage::Structured;
        buffers.push_back(graph.CreateBuffer(desc));
        EXPECT_TRUE(buffers.back().IsValid());
    }
}

struct SimplePassData
{
    RGTextureHandle colorTarget;
};

TEST(RenderGraphValidation, SinglePass)
{
    RenderGraph graph;

    RHITextureDesc texDesc = RHITextureDesc::RenderTarget(1920, 1080, RHIFormat::RGBA16_FLOAT);
    auto texture = graph.CreateTexture(texDesc);

    graph.AddPass<SimplePassData>(
        "SimplePass",
        RenderGraphPassType::Graphics,
        [&](RenderGraphBuilder& builder, SimplePassData& data)
        {
            data.colorTarget = builder.Write(texture, RHIResourceState::RenderTarget);
        },
        [](const SimplePassData&, RHICommandContext&)
        {
            // No-op execution
        });

    graph.SetExportState(texture, RHIResourceState::ShaderResource);
    graph.Compile();
}

TEST(RenderGraphValidation, DepthTextureArrayLayerWritesFullReadAndExportUseDepthAspect)
{
    FakeDevice device;
    RenderGraph graph;
    graph.SetDevice(&device);

    RHITextureDesc shadowDesc = RHITextureDesc::DepthStencil(64, 64, RHIFormat::D32_FLOAT);
    shadowDesc.arraySize = 2;
    shadowDesc.debugName = "DepthArrayForAspectValidation";
    RGTextureHandle shadowArray = graph.CreateTexture(shadowDesc);

    auto makeDepthLayer = [shadowArray](uint32 layer)
    {
        RGTextureHandle handle = shadowArray;
        handle.hasSubresourceRange = true;
        handle.subresourceRange = RHISubresourceRange{0, 1, layer, 1, RHITextureAspect::Depth};
        return handle;
    };

    struct DepthLayerPassData
    {
        RGTextureHandle layer;
    };
    struct DepthReadPassData
    {
        RGTextureHandle shadowArray;
    };

    graph.AddPass<DepthLayerPassData>(
        "WriteDepthLayer0",
        RenderGraphPassType::Graphics,
        [&](RenderGraphBuilder& builder, DepthLayerPassData& data)
        {
            data.layer = makeDepthLayer(0);
            builder.SetDepthStencil(data.layer, true, false);
        },
        [](const DepthLayerPassData&, RHICommandContext&)
        {
        });

    graph.AddPass<DepthLayerPassData>(
        "WriteDepthLayer1",
        RenderGraphPassType::Graphics,
        [&](RenderGraphBuilder& builder, DepthLayerPassData& data)
        {
            data.layer = makeDepthLayer(1);
            builder.SetDepthStencil(data.layer, true, false);
        },
        [](const DepthLayerPassData&, RHICommandContext&)
        {
        });

    graph.AddPass<DepthReadPassData>(
        "ReadFullDepthArray",
        RenderGraphPassType::Graphics,
        [&](RenderGraphBuilder& builder, DepthReadPassData& data)
        {
            data.shadowArray = shadowArray;
            data.shadowArray.hasSubresourceRange = true;
            data.shadowArray.subresourceRange = RHISubresourceRange{0, RVX_ALL_MIPS, 0, RVX_ALL_LAYERS, RHITextureAspect::Depth};
            builder.Read(data.shadowArray, RHIShaderStage::Pixel);
        },
        [](const DepthReadPassData&, RHICommandContext&)
        {
        });

    graph.SetExportState(shadowArray, RHIResourceState::ShaderResource);
    graph.Compile();

    const auto& stats = graph.GetCompileStats();
    EXPECT_TRUE(stats.compileValid);

    FakeCommandContext ctx;
    graph.Execute(ctx);

    ASSERT_FALSE(ctx.textureBarriers.empty());
    for (const RHITextureBarrier& barrier : ctx.textureBarriers)
    {
        EXPECT_EQ(barrier.subresourceRange.aspect, RHITextureAspect::Depth);
    }
}

struct MultiPassData
{
    RGTextureHandle texture;
    RGBufferHandle buffer;
};

TEST(RenderGraphValidation, PassChain)
{
    RenderGraph graph;

    // Create resources
    RHITextureDesc texDesc = RHITextureDesc::RenderTarget(1920, 1080, RHIFormat::RGBA16_FLOAT);
    auto gBuffer = graph.CreateTexture(texDesc);
    auto lighting = graph.CreateTexture(texDesc);
    auto final = graph.CreateTexture(texDesc);

    // GBuffer pass
    graph.AddPass<SimplePassData>(
        "GBuffer",
        RenderGraphPassType::Graphics,
        [&](RenderGraphBuilder& builder, SimplePassData& data)
        {
            data.colorTarget = builder.Write(gBuffer, RHIResourceState::RenderTarget);
        },
        [](const SimplePassData&, RHICommandContext&) {});

    // Lighting pass - reads GBuffer, writes lighting
    struct LightingPassData
    {
        RGTextureHandle input;
        RGTextureHandle output;
    };

    graph.AddPass<LightingPassData>(
        "Lighting",
        RenderGraphPassType::Graphics,
        [&](RenderGraphBuilder& builder, LightingPassData& data)
        {
            data.input = builder.Read(gBuffer);
            data.output = builder.Write(lighting, RHIResourceState::RenderTarget);
        },
        [](const LightingPassData&, RHICommandContext&) {});

    // Final pass - reads lighting, writes final
    graph.AddPass<LightingPassData>(
        "Final",
        RenderGraphPassType::Graphics,
        [&](RenderGraphBuilder& builder, LightingPassData& data)
        {
            data.input = builder.Read(lighting);
            data.output = builder.Write(final, RHIResourceState::RenderTarget);
        },
        [](const LightingPassData&, RHICommandContext&) {});

    graph.SetExportState(final, RHIResourceState::Present);
    graph.Compile();
}

TEST(RenderGraphValidation, PassCulling)
{
    RenderGraph graph;

    RHITextureDesc texDesc = RHITextureDesc::RenderTarget(256, 256, RHIFormat::RGBA8_UNORM);
    auto usedTexture = graph.CreateTexture(texDesc);
    auto unusedTexture = graph.CreateTexture(texDesc);

    // This pass writes to usedTexture - should NOT be culled
    graph.AddPass<SimplePassData>(
        "UsedPass",
        RenderGraphPassType::Graphics,
        [&](RenderGraphBuilder& builder, SimplePassData& data)
        {
            data.colorTarget = builder.Write(usedTexture, RHIResourceState::RenderTarget);
        },
        [](const SimplePassData&, RHICommandContext&) {});

    // This pass writes to unusedTexture - SHOULD be culled
    graph.AddPass<SimplePassData>(
        "UnusedPass",
        RenderGraphPassType::Graphics,
        [&](RenderGraphBuilder& builder, SimplePassData& data)
        {
            data.colorTarget = builder.Write(unusedTexture, RHIResourceState::RenderTarget);
        },
        [](const SimplePassData&, RHICommandContext&) {});

    // Only export usedTexture
    graph.SetExportState(usedTexture, RHIResourceState::ShaderResource);
    graph.Compile();

    // Check that unused pass was culled
    const auto& stats = graph.GetCompileStats();
    EXPECT_TRUE(stats.culledPasses > 0);
}

TEST(RenderGraphValidation, MemoryAliasing)
{
    RenderGraph graph;
    graph.SetMemoryAliasingEnabled(true);

    RHITextureDesc texDesc = RHITextureDesc::RenderTarget(1024, 1024, RHIFormat::RGBA16_FLOAT);

    // Create textures with non-overlapping lifetimes
    auto texA = graph.CreateTexture(texDesc);
    auto texB = graph.CreateTexture(texDesc);
    auto texC = graph.CreateTexture(texDesc);
    auto final = graph.CreateTexture(texDesc);

    // Pass 1: Write A
    graph.AddPass<SimplePassData>(
        "PassA",
        RenderGraphPassType::Graphics,
        [&](RenderGraphBuilder& builder, SimplePassData& data)
        {
            data.colorTarget = builder.Write(texA, RHIResourceState::RenderTarget);
        },
        [](const SimplePassData&, RHICommandContext&) {});

    // Pass 2: Read A, Write B (A's lifetime ends)
    struct TwoTexturePass
    {
        RGTextureHandle input;
        RGTextureHandle output;
    };

    graph.AddPass<TwoTexturePass>(
        "PassB",
        RenderGraphPassType::Graphics,
        [&](RenderGraphBuilder& builder, TwoTexturePass& data)
        {
            data.input = builder.Read(texA);
            data.output = builder.Write(texB, RHIResourceState::RenderTarget);
        },
        [](const TwoTexturePass&, RHICommandContext&) {});

    // Pass 3: Read B, Write C (B's lifetime ends)
    graph.AddPass<TwoTexturePass>(
        "PassC",
        RenderGraphPassType::Graphics,
        [&](RenderGraphBuilder& builder, TwoTexturePass& data)
        {
            data.input = builder.Read(texB);
            data.output = builder.Write(texC, RHIResourceState::RenderTarget);
        },
        [](const TwoTexturePass&, RHICommandContext&) {});

    // Pass 4: Read C, Write final
    graph.AddPass<TwoTexturePass>(
        "FinalPass",
        RenderGraphPassType::Graphics,
        [&](RenderGraphBuilder& builder, TwoTexturePass& data)
        {
            data.input = builder.Read(texC);
            data.output = builder.Write(final, RHIResourceState::RenderTarget);
        },
        [](const TwoTexturePass&, RHICommandContext&) {});

    graph.SetExportState(final, RHIResourceState::Present);
    graph.Compile();

    const auto& stats = graph.GetCompileStats();

    EXPECT_FALSE(graph.IsMemoryAliasingEnabled());
    EXPECT_FALSE(stats.memoryAliasingEnabled);
    EXPECT_TRUE(stats.memoryAliasingUnsupportedRequested);
    EXPECT_FALSE(stats.explicitAliasingBarriersSupported);
    EXPECT_EQ(stats.aliasedTextureCount, 0u);
    EXPECT_EQ(stats.aliasedBufferCount, 0u);
}

TEST(RenderGraphValidation, ComputePass)
{
    RenderGraph graph;

    RHIBufferDesc bufDesc;
    bufDesc.size = 1024 * 1024;
    bufDesc.usage = RHIBufferUsage::UnorderedAccess | RHIBufferUsage::Structured;
    auto buffer = graph.CreateBuffer(bufDesc);

    struct ComputePassData
    {
        RGBufferHandle buffer;
    };

    graph.AddPass<ComputePassData>(
        "ComputePass",
        RenderGraphPassType::Compute,
        [&](RenderGraphBuilder& builder, ComputePassData& data)
        {
            data.buffer = builder.Write(buffer, RHIResourceState::UnorderedAccess);
        },
        [](const ComputePassData&, RHICommandContext&) {});

    graph.SetExportState(buffer, RHIResourceState::ShaderResource);
    graph.Compile();
}

TEST(RenderGraphValidation, MixedPasses)
{
    RenderGraph graph;

    RHITextureDesc texDesc = RHITextureDesc::RenderTarget(512, 512, RHIFormat::RGBA16_FLOAT);
    texDesc.usage = RHITextureUsage::RenderTarget | RHITextureUsage::ShaderResource | RHITextureUsage::UnorderedAccess;
    auto texture = graph.CreateTexture(texDesc);

    RHIBufferDesc bufDesc;
    bufDesc.size = 65536;
    bufDesc.usage = RHIBufferUsage::UnorderedAccess | RHIBufferUsage::Structured;
    auto buffer = graph.CreateBuffer(bufDesc);

    // Compute pass
    struct ComputeData { RGBufferHandle buf; };
    graph.AddPass<ComputeData>(
        "ComputePrep",
        RenderGraphPassType::Compute,
        [&](RenderGraphBuilder& builder, ComputeData& data)
        {
            data.buf = builder.Write(buffer, RHIResourceState::UnorderedAccess);
        },
        [](const ComputeData&, RHICommandContext&) {});

    // Graphics pass using compute output
    struct GraphicsData { RGBufferHandle buf; RGTextureHandle tex; };
    graph.AddPass<GraphicsData>(
        "GraphicsRender",
        RenderGraphPassType::Graphics,
        [&](RenderGraphBuilder& builder, GraphicsData& data)
        {
            data.buf = builder.Read(buffer);
            data.tex = builder.Write(texture, RHIResourceState::RenderTarget);
        },
        [](const GraphicsData&, RHICommandContext&) {});

    // Post-process compute pass
    struct PostData { RGTextureHandle tex; };
    graph.AddPass<PostData>(
        "ComputePost",
        RenderGraphPassType::Compute,
        [&](RenderGraphBuilder& builder, PostData& data)
        {
            data.tex = builder.Write(texture, RHIResourceState::UnorderedAccess);
        },
        [](const PostData&, RHICommandContext&) {});

    graph.SetExportState(texture, RHIResourceState::ShaderResource);
    graph.Compile();

    const auto& stats = graph.GetCompileStats();
    EXPECT_EQ(stats.totalPasses, 3u);
    EXPECT_EQ(stats.culledPasses, 0u);
}

TEST(RenderGraphValidation, SubresourceTracking)
{
    RenderGraph graph;

    // Create a texture with multiple mip levels
    RHITextureDesc texDesc;
    texDesc.width = 1024;
    texDesc.height = 1024;
    texDesc.mipLevels = 4;
    texDesc.format = RHIFormat::RGBA16_FLOAT;
    texDesc.usage = RHITextureUsage::RenderTarget | RHITextureUsage::ShaderResource;
    auto texture = graph.CreateTexture(texDesc);

    // Write to mip 0
    struct MipPassData { RGTextureHandle mip; };
    graph.AddPass<MipPassData>(
        "WriteMip0",
        RenderGraphPassType::Graphics,
        [&](RenderGraphBuilder& builder, MipPassData& data)
        {
            data.mip = builder.WriteMip(texture, 0);
        },
        [](const MipPassData&, RHICommandContext&) {});

    // Read mip 0, write mip 1
    struct MipCopyData { RGTextureHandle src; RGTextureHandle dst; };
    graph.AddPass<MipCopyData>(
        "CopyMip0to1",
        RenderGraphPassType::Graphics,
        [&](RenderGraphBuilder& builder, MipCopyData& data)
        {
            data.src = builder.ReadMip(texture, 0);
            data.dst = builder.WriteMip(texture, 1);
        },
        [](const MipCopyData&, RHICommandContext&) {});

    graph.SetExportState(texture, RHIResourceState::ShaderResource);
    graph.Compile();
}

TEST(RenderGraphValidation, BufferRanges)
{
    RenderGraph graph;

    RHIBufferDesc bufDesc;
    bufDesc.size = 1024 * 1024;  // 1 MB
    bufDesc.usage = RHIBufferUsage::Structured | RHIBufferUsage::ShaderResource;
    FakeBuffer backingBuffer(bufDesc);
    auto buffer = graph.ImportBuffer(&backingBuffer, RHIResourceState::ShaderResource);

    struct RangePassData { RGBufferHandle range; };

    // Pass 1: Read first half
    graph.AddPass<RangePassData>(
        "ReadFirstHalf",
        RenderGraphPassType::Compute,
        [&](RenderGraphBuilder& builder, RangePassData& data)
        {
            data.range = builder.Read(buffer.Range(0, 512 * 1024));
        },
        [](const RangePassData&, RHICommandContext&) {});

    // Pass 2: Read second half
    graph.AddPass<RangePassData>(
        "ReadSecondHalf",
        RenderGraphPassType::Compute,
        [&](RenderGraphBuilder& builder, RangePassData& data)
        {
            data.range = builder.Read(buffer.Range(512 * 1024, 512 * 1024));
        },
        [](const RangePassData&, RHICommandContext&) {});

    graph.SetExportState(buffer, RHIResourceState::ShaderResource);
    graph.Compile();
}

TEST(RenderGraphValidation, CrossPassSubresourceBarriersAreNotDropped)
{
    RenderGraph graph;

    RHITextureDesc texDesc;
    texDesc.width = 256;
    texDesc.height = 256;
    texDesc.mipLevels = 2;
    texDesc.format = RHIFormat::RGBA8_UNORM;
    texDesc.usage = RHITextureUsage::RenderTarget | RHITextureUsage::ShaderResource;
    FakeTexture texture(texDesc);

    auto imported = graph.ImportTexture(&texture, RHIResourceState::Common);

    struct MipPassData
    {
        RGTextureHandle mip;
    };

    graph.AddPass<MipPassData>(
        "WriteMip0",
        RenderGraphPassType::Graphics,
        [&](RenderGraphBuilder& builder, MipPassData& data)
        {
            data.mip = builder.WriteMip(imported, 0);
        },
        [](const MipPassData&, RHICommandContext&) {});

    graph.AddPass<MipPassData>(
        "WriteMip1",
        RenderGraphPassType::Graphics,
        [&](RenderGraphBuilder& builder, MipPassData& data)
        {
            data.mip = builder.WriteMip(imported, 1);
        },
        [](const MipPassData&, RHICommandContext&) {});

    graph.Compile();

    const auto& stats = graph.GetCompileStats();
    EXPECT_EQ(stats.totalPasses, 2u);
    EXPECT_EQ(stats.textureBarrierCount, 2u);
    EXPECT_EQ(stats.crossPassMergedBarrierCount, 0u);
}

TEST(RenderGraphValidation, CrossPassBufferRangeBarriersAreNotDropped)
{
    RenderGraph graph;

    RHIBufferDesc bufferDesc;
    bufferDesc.size = 1024;
    bufferDesc.usage = RHIBufferUsage::Structured | RHIBufferUsage::UnorderedAccess;
    FakeBuffer buffer(bufferDesc);

    auto imported = graph.ImportBuffer(&buffer, RHIResourceState::Common);

    struct RangePassData
    {
        RGBufferHandle range;
    };

    graph.AddPass<RangePassData>(
        "WriteFirstHalf",
        RenderGraphPassType::Compute,
        [&](RenderGraphBuilder& builder, RangePassData& data)
        {
            data.range = builder.Write(imported.Range(0, 512), RHIResourceState::UnorderedAccess);
        },
        [](const RangePassData&, RHICommandContext&) {});

    graph.AddPass<RangePassData>(
        "WriteSecondHalf",
        RenderGraphPassType::Compute,
        [&](RenderGraphBuilder& builder, RangePassData& data)
        {
            data.range = builder.Write(imported.Range(512, 512), RHIResourceState::UnorderedAccess);
        },
        [](const RangePassData&, RHICommandContext&) {});

    graph.Compile();

    const auto& stats = graph.GetCompileStats();
    EXPECT_EQ(stats.totalPasses, 2u);
    EXPECT_EQ(stats.bufferBarrierCount, 2u);
    EXPECT_EQ(stats.crossPassMergedBarrierCount, 0u);
}

TEST(RenderGraphValidation, ReadBeforeWriteHazardPreservesExecutionOrder)
{
    RenderGraph graph;

    // Imported resources start initialized from their external state. This test
    // verifies a valid imported read before a later graph write, not a transient
    // read-before-write hazard.
    RHITextureDesc texDesc = RHITextureDesc::RenderTarget(128, 128, RHIFormat::RGBA8_UNORM);
    FakeTexture textureA(texDesc);
    FakeTexture textureB(texDesc);
    FakeTexture textureC(texDesc);

    auto a = graph.ImportTexture(&textureA, RHIResourceState::ShaderResource);
    auto b = graph.ImportTexture(&textureB, RHIResourceState::Common);
    auto c = graph.ImportTexture(&textureC, RHIResourceState::Common);

    std::vector<std::string> executed;

    graph.AddPass<SimplePassData>(
        "ProduceB",
        RenderGraphPassType::Graphics,
        [&](RenderGraphBuilder& builder, SimplePassData& data)
        {
            data.colorTarget = builder.Write(b, RHIResourceState::RenderTarget);
        },
        [&executed](const SimplePassData&, RHICommandContext&)
        {
            executed.push_back("ProduceB");
        });

    struct ReadAWriteCData
    {
        RGTextureHandle inputA;
        RGTextureHandle inputB;
        RGTextureHandle outputC;
    };

    graph.AddPass<ReadAWriteCData>(
        "ReadAWriteC",
        RenderGraphPassType::Graphics,
        [&](RenderGraphBuilder& builder, ReadAWriteCData& data)
        {
            data.inputB = builder.Read(b);
            data.inputA = builder.Read(a);
            data.outputC = builder.Write(c, RHIResourceState::RenderTarget);
        },
        [&executed](const ReadAWriteCData&, RHICommandContext&)
        {
            executed.push_back("ReadAWriteC");
        });

    graph.AddPass<SimplePassData>(
        "WriteA",
        RenderGraphPassType::Graphics,
        [&](RenderGraphBuilder& builder, SimplePassData& data)
        {
            data.colorTarget = builder.Write(a, RHIResourceState::RenderTarget);
        },
        [&executed](const SimplePassData&, RHICommandContext&)
        {
            executed.push_back("WriteA");
        });

    graph.SetExportState(c, RHIResourceState::ShaderResource);
    graph.SetExportState(a, RHIResourceState::ShaderResource);
    graph.Compile();

    const auto& stats = graph.GetCompileStats();
    EXPECT_TRUE(stats.compileValid);
    EXPECT_EQ(stats.readBeforeWriteHazardCount, 0u);
    EXPECT_EQ(stats.uninitializedExportCount, 0u);

    FakeCommandContext ctx;
    graph.Execute(ctx);

    EXPECT_EQ(executed.size(), static_cast<size_t>(3));
    EXPECT_EQ(executed[0], std::string("ProduceB"));
    EXPECT_EQ(executed[1], std::string("ReadAWriteC"));
    EXPECT_EQ(executed[2], std::string("WriteA"));
}

TEST(RenderGraphValidation, TransientTextureReadBeforeWriteFailsCompileAndDoesNotExecute)
{
    RenderGraph graph;

    RHITextureDesc texDesc = RHITextureDesc::RenderTarget(128, 128, RHIFormat::RGBA8_UNORM);
    auto input = graph.CreateTexture(texDesc);
    auto output = graph.CreateTexture(texDesc);
    bool executed = false;

    struct TextureHazardData
    {
        RGTextureHandle input;
        RGTextureHandle output;
    };

    graph.AddPass<TextureHazardData>(
        "ReadUnwrittenTexture",
        RenderGraphPassType::Graphics,
        [&](RenderGraphBuilder& builder, TextureHazardData& data)
        {
            data.input = builder.Read(input);
            data.output = builder.Write(output, RHIResourceState::RenderTarget);
        },
        [&](const TextureHazardData&, RHICommandContext&)
        {
            executed = true;
        });

    graph.SetExportState(output, RHIResourceState::ShaderResource);
    graph.Compile();

    const auto& stats = graph.GetCompileStats();
    EXPECT_FALSE(stats.compileValid);
    EXPECT_EQ(stats.readBeforeWriteHazardCount, 1u);
    EXPECT_EQ(stats.uninitializedExportCount, 0u);
    EXPECT_EQ(stats.validationErrorCount, 1u);
    EXPECT_FALSE(stats.executionOrderFallbackUsed);

    FakeCommandContext ctx;
    graph.Execute(ctx);
    EXPECT_FALSE(executed);
}

TEST(RenderGraphValidation, TransientBufferReadBeforeWriteFailsCompile)
{
    RenderGraph graph;

    RHIBufferDesc bufferDesc;
    bufferDesc.size = 1024;
    bufferDesc.usage = RHIBufferUsage::Structured | RHIBufferUsage::UnorderedAccess;
    auto input = graph.CreateBuffer(bufferDesc);
    auto output = graph.CreateBuffer(bufferDesc);
    bool executed = false;

    struct BufferHazardData
    {
        RGBufferHandle input;
        RGBufferHandle output;
    };

    graph.AddPass<BufferHazardData>(
        "ReadUnwrittenBuffer",
        RenderGraphPassType::Compute,
        [&](RenderGraphBuilder& builder, BufferHazardData& data)
        {
            data.input = builder.Read(input);
            data.output = builder.Write(output, RHIResourceState::UnorderedAccess);
        },
        [&](const BufferHazardData&, RHICommandContext&)
        {
            executed = true;
        });

    graph.SetExportState(output, RHIResourceState::ShaderResource);
    graph.Compile();

    const auto& stats = graph.GetCompileStats();
    EXPECT_FALSE(stats.compileValid);
    EXPECT_EQ(stats.readBeforeWriteHazardCount, 1u);
    EXPECT_EQ(stats.uninitializedExportCount, 0u);
    EXPECT_EQ(stats.validationErrorCount, 1u);

    FakeCommandContext ctx;
    graph.Execute(ctx);
    EXPECT_FALSE(executed);
}

TEST(RenderGraphValidation, TransientReadWriteBeforeInitializationFailsCompile)
{
    RenderGraph graph;

    RHITextureDesc texDesc = RHITextureDesc::RenderTarget(64, 64, RHIFormat::RGBA8_UNORM);
    texDesc.usage = RHITextureUsage::RenderTarget | RHITextureUsage::UnorderedAccess;
    auto texture = graph.CreateTexture(texDesc);

    RHIBufferDesc bufferDesc;
    bufferDesc.size = 1024;
    bufferDesc.usage = RHIBufferUsage::Structured | RHIBufferUsage::UnorderedAccess;
    auto buffer = graph.CreateBuffer(bufferDesc);

    struct TextureReadWriteData
    {
        RGTextureHandle texture;
    };
    struct BufferReadWriteData
    {
        RGBufferHandle buffer;
    };

    graph.AddPass<TextureReadWriteData>(
        "ReadWriteUnwrittenTexture",
        RenderGraphPassType::Compute,
        [&](RenderGraphBuilder& builder, TextureReadWriteData& data)
        {
            data.texture = builder.ReadWrite(texture);
        },
        [](const TextureReadWriteData&, RHICommandContext&) {});

    graph.AddPass<BufferReadWriteData>(
        "ReadWriteUnwrittenBuffer",
        RenderGraphPassType::Compute,
        [&](RenderGraphBuilder& builder, BufferReadWriteData& data)
        {
            data.buffer = builder.ReadWrite(buffer);
        },
        [](const BufferReadWriteData&, RHICommandContext&) {});

    graph.SetExportState(texture, RHIResourceState::ShaderResource);
    graph.SetExportState(buffer, RHIResourceState::ShaderResource);
    graph.Compile();

    const auto& stats = graph.GetCompileStats();
    EXPECT_FALSE(stats.compileValid);
    EXPECT_EQ(stats.readBeforeWriteHazardCount, 2u);
    EXPECT_EQ(stats.uninitializedExportCount, 0u);
    EXPECT_EQ(stats.validationErrorCount, 2u);
}

TEST(RenderGraphValidation, TransientExportWithoutProducerFailsCompile)
{
    RenderGraph graph;

    RHITextureDesc texDesc = RHITextureDesc::RenderTarget(64, 64, RHIFormat::RGBA8_UNORM);
    auto texture = graph.CreateTexture(texDesc);

    RHIBufferDesc bufferDesc;
    bufferDesc.size = 1024;
    bufferDesc.usage = RHIBufferUsage::Structured;
    auto buffer = graph.CreateBuffer(bufferDesc);

    graph.SetExportState(texture, RHIResourceState::ShaderResource);
    graph.SetExportState(buffer, RHIResourceState::ShaderResource);
    graph.Compile();

    const auto& stats = graph.GetCompileStats();
    EXPECT_FALSE(stats.compileValid);
    EXPECT_EQ(stats.readBeforeWriteHazardCount, 0u);
    EXPECT_EQ(stats.uninitializedExportCount, 2u);
    EXPECT_EQ(stats.validationErrorCount, 2u);
}

TEST(RenderGraphValidation, CulledTransientReadBeforeWriteDoesNotInvalidateGraph)
{
    RenderGraph graph;

    RHITextureDesc texDesc = RHITextureDesc::RenderTarget(64, 64, RHIFormat::RGBA8_UNORM);
    auto unusedInput = graph.CreateTexture(texDesc);
    auto unusedOutput = graph.CreateTexture(texDesc);
    auto finalOutput = graph.CreateTexture(texDesc);

    struct TextureHazardData
    {
        RGTextureHandle input;
        RGTextureHandle output;
    };

    graph.AddPass<TextureHazardData>(
        "CulledReadUnwrittenTexture",
        RenderGraphPassType::Graphics,
        [&](RenderGraphBuilder& builder, TextureHazardData& data)
        {
            data.input = builder.Read(unusedInput);
            data.output = builder.Write(unusedOutput, RHIResourceState::RenderTarget);
        },
        [](const TextureHazardData&, RHICommandContext&) {});

    graph.AddPass<SimplePassData>(
        "ProduceFinal",
        RenderGraphPassType::Graphics,
        [&](RenderGraphBuilder& builder, SimplePassData& data)
        {
            data.colorTarget = builder.Write(finalOutput, RHIResourceState::RenderTarget);
        },
        [](const SimplePassData&, RHICommandContext&) {});

    graph.SetExportState(finalOutput, RHIResourceState::ShaderResource);
    graph.Compile();

    const auto& stats = graph.GetCompileStats();
    EXPECT_TRUE(stats.compileValid);
    EXPECT_EQ(stats.readBeforeWriteHazardCount, 0u);
    EXPECT_EQ(stats.uninitializedExportCount, 0u);
    EXPECT_EQ(stats.validationErrorCount, 0u);
    EXPECT_EQ(stats.culledPasses, 1u);
}

TEST(RenderGraphValidation, LifetimeHazardStatsResetAfterClear)
{
    RenderGraph graph;

    RHITextureDesc texDesc = RHITextureDesc::RenderTarget(64, 64, RHIFormat::RGBA8_UNORM);
    auto unwritten = graph.CreateTexture(texDesc);
    graph.SetExportState(unwritten, RHIResourceState::ShaderResource);
    graph.Compile();

    EXPECT_FALSE(graph.GetCompileStats().compileValid);
    EXPECT_EQ(graph.GetCompileStats().uninitializedExportCount, 1u);

    graph.Clear();
    auto written = graph.CreateTexture(texDesc);
    graph.AddPass<SimplePassData>(
        "ProduceWrittenTexture",
        RenderGraphPassType::Graphics,
        [&](RenderGraphBuilder& builder, SimplePassData& data)
        {
            data.colorTarget = builder.Write(written, RHIResourceState::RenderTarget);
        },
        [](const SimplePassData&, RHICommandContext&) {});

    graph.SetExportState(written, RHIResourceState::ShaderResource);
    graph.Compile();

    const auto& stats = graph.GetCompileStats();
    EXPECT_TRUE(stats.compileValid);
    EXPECT_EQ(stats.readBeforeWriteHazardCount, 0u);
    EXPECT_EQ(stats.uninitializedExportCount, 0u);
    EXPECT_EQ(stats.validationErrorCount, 0u);
}

TEST(RenderGraphValidation, ExecuteAsyncFallsBackToGraphicsUntilQueueSchedulerExists)
{
    RenderGraph graph;

    RHIBufferDesc bufferDesc;
    bufferDesc.size = 1024;
    bufferDesc.usage = RHIBufferUsage::Structured | RHIBufferUsage::UnorderedAccess;
    FakeBuffer buffer(bufferDesc);

    auto imported = graph.ImportBuffer(&buffer, RHIResourceState::Common);

    FakeCommandContext graphicsCtx;
    FakeCommandContext computeCtx;
    FakeFence computeFence;
    bool ranOnGraphicsContext = false;
    bool ranOnComputeContext = false;

    struct ComputeData
    {
        RGBufferHandle buffer;
    };

    graph.AddPass<ComputeData>(
        "ComputeWork",
        RenderGraphPassType::Compute,
        [&](RenderGraphBuilder& builder, ComputeData& data)
        {
            data.buffer = builder.Write(imported, RHIResourceState::UnorderedAccess);
        },
        [&](const ComputeData&, RHICommandContext& ctx)
        {
            ranOnGraphicsContext = (&ctx == &graphicsCtx);
            ranOnComputeContext = (&ctx == &computeCtx);
        });

    graph.SetExportState(imported, RHIResourceState::ShaderResource);
    graph.Compile();
    graph.ExecuteAsync(graphicsCtx, &computeCtx, &computeFence, 1);

    const auto& stats = graph.GetCompileStats();
    EXPECT_FALSE(stats.asyncComputeSupported);
    EXPECT_TRUE(stats.asyncFallbackUsed);
    EXPECT_TRUE(ranOnGraphicsContext);
    EXPECT_FALSE(ranOnComputeContext);
}

TEST(RenderGraphValidation, ClearAndRecompile)
{
    RenderGraph graph;

    // First frame
    RHITextureDesc texDesc = RHITextureDesc::RenderTarget(512, 512, RHIFormat::RGBA8_UNORM);
    auto tex1 = graph.CreateTexture(texDesc);

    graph.AddPass<SimplePassData>(
        "Frame1Pass",
        RenderGraphPassType::Graphics,
        [&](RenderGraphBuilder& builder, SimplePassData& data)
        {
            data.colorTarget = builder.Write(tex1, RHIResourceState::RenderTarget);
        },
        [](const SimplePassData&, RHICommandContext&) {});

    graph.SetExportState(tex1, RHIResourceState::Present);
    graph.Compile();

    // Clear for next frame
    graph.Clear();

    // Second frame - different setup
    auto tex2 = graph.CreateTexture(texDesc);
    auto tex3 = graph.CreateTexture(texDesc);

    struct TwoPassData { RGTextureHandle t1; RGTextureHandle t2; };
    graph.AddPass<TwoPassData>(
        "Frame2Pass1",
        RenderGraphPassType::Graphics,
        [&](RenderGraphBuilder& builder, TwoPassData& data)
        {
            data.t1 = builder.Write(tex2, RHIResourceState::RenderTarget);
        },
        [](const TwoPassData&, RHICommandContext&) {});

    graph.AddPass<TwoPassData>(
        "Frame2Pass2",
        RenderGraphPassType::Graphics,
        [&](RenderGraphBuilder& builder, TwoPassData& data)
        {
            data.t1 = builder.Read(tex2);
            data.t2 = builder.Write(tex3, RHIResourceState::RenderTarget);
        },
        [](const TwoPassData&, RHICommandContext&) {});

    graph.SetExportState(tex3, RHIResourceState::Present);
    graph.Compile();

    const auto& stats = graph.GetCompileStats();
    EXPECT_EQ(stats.totalPasses, 2u);
}

TEST(RenderGraphValidation, InvalidTextureUsageIsReported)
{
    RenderGraph graph;

    graph.AddPass<SimplePassData>(
        "InvalidTextureUsage",
        RenderGraphPassType::Graphics,
        [](RenderGraphBuilder& builder, SimplePassData& data)
        {
            data.colorTarget = builder.Write(RGTextureHandle{777}, RHIResourceState::RenderTarget);
        },
        [](const SimplePassData&, RHICommandContext&) {});

    graph.Compile();

    const auto& stats = graph.GetCompileStats();
    EXPECT_EQ(stats.totalPasses, 1u);
    EXPECT_FALSE(stats.compileValid);
    EXPECT_EQ(stats.invalidResourceUsageCount, 1u);
    EXPECT_EQ(stats.validationErrorCount, 1u);
    EXPECT_FALSE(stats.executionOrderFallbackUsed);
}

TEST(RenderGraphValidation, InvalidBufferUsageIsReported)
{
    RenderGraph graph;

    struct InvalidBufferPassData
    {
        RGBufferHandle buffer;
    };

    graph.AddPass<InvalidBufferPassData>(
        "InvalidBufferUsage",
        RenderGraphPassType::Compute,
        [](RenderGraphBuilder& builder, InvalidBufferPassData& data)
        {
            data.buffer = builder.Write(RGBufferHandle{888}, RHIResourceState::UnorderedAccess);
        },
        [](const InvalidBufferPassData&, RHICommandContext&) {});

    graph.Compile();

    const auto& stats = graph.GetCompileStats();
    EXPECT_EQ(stats.totalPasses, 1u);
    EXPECT_FALSE(stats.compileValid);
    EXPECT_EQ(stats.invalidResourceUsageCount, 1u);
    EXPECT_EQ(stats.validationErrorCount, 1u);
    EXPECT_FALSE(stats.executionOrderFallbackUsed);
}

TEST(RenderGraphValidation, EmptyPassUsageIsReported)
{
    RenderGraph graph;

    graph.AddPass<SimplePassData>(
        "EmptyUsage",
        RenderGraphPassType::Graphics,
        [](RenderGraphBuilder&, SimplePassData&) {},
        [](const SimplePassData&, RHICommandContext&) {});

    graph.Compile();

    const auto& stats = graph.GetCompileStats();
    EXPECT_EQ(stats.totalPasses, 1u);
    EXPECT_TRUE(stats.compileValid);
    EXPECT_EQ(stats.emptyPassUsageCount, 1u);
    EXPECT_EQ(stats.validationWarningCount, 1u);
}

TEST(RenderGraphValidation, ShaderStageMismatchWarningsDoNotInvalidateCompile)
{
    FakeDevice device;
    RenderGraph graph;
    graph.SetDevice(&device);

    RHITextureRef inputTexture =
        device.CreateTexture(RHITextureDesc::Texture2D(32, 32, RHIFormat::RGBA16_FLOAT));
    ASSERT_NE(inputTexture.Get(), nullptr);

    RGTextureHandle input = graph.ImportTexture(inputTexture.Get(), RHIResourceState::ShaderResource);
    RGTextureHandle computeOutput =
        graph.CreateTexture(RHITextureDesc::RenderTarget(32, 32, RHIFormat::RGBA16_FLOAT));
    RGTextureHandle rayTracingOutput =
        graph.CreateTexture(RHITextureDesc::RenderTarget(32, 32, RHIFormat::RGBA16_FLOAT));

    struct StageMismatchData
    {
        RGTextureHandle input;
        RGTextureHandle output;
    };

    graph.AddPass<StageMismatchData>(
        "ComputeReadsDefaultGraphicsStage",
        RenderGraphPassType::Compute,
        [input, computeOutput](RenderGraphBuilder& builder, StageMismatchData& data)
        {
            data.input = builder.Read(input);
            data.output = builder.Write(computeOutput, RHIResourceState::UnorderedAccess);
        },
        [](const StageMismatchData&, RHICommandContext&) {});

    graph.AddPass<StageMismatchData>(
        "RayTracingReadsPixelStage",
        RenderGraphPassType::RayTracing,
        [input, rayTracingOutput](RenderGraphBuilder& builder, StageMismatchData& data)
        {
            data.input = builder.Read(input, RHIResourceState::ShaderResource, RHIShaderStage::Pixel);
            data.output = builder.Write(rayTracingOutput, RHIResourceState::UnorderedAccess);
        },
        [](const StageMismatchData&, RHICommandContext&) {});

    graph.SetExportState(computeOutput, RHIResourceState::ShaderResource);
    graph.SetExportState(rayTracingOutput, RHIResourceState::ShaderResource);
    graph.Compile();

    const auto& stats = graph.GetCompileStats();
    EXPECT_TRUE(stats.compileValid);
    EXPECT_EQ(stats.shaderStageMismatchUsageCount, 2u);
    EXPECT_EQ(stats.validationWarningCount, 2u);
    EXPECT_EQ(stats.validationErrorCount, 0u);

    const std::vector<std::string>& diagnostics = graph.GetCompileDiagnostics();
    ASSERT_EQ(diagnostics.size(), 2u);
    EXPECT_NE(diagnostics[0].find("ComputeReadsDefaultGraphicsStage"), std::string::npos);
    EXPECT_NE(diagnostics[1].find("RayTracingReadsPixelStage"), std::string::npos);
}

TEST(RenderGraphValidation, IncompatiblePassStateIsReported)
{
    RenderGraph graph;

    graph.AddPass<SimplePassData>(
        "ComputeRenderTargetState",
        RenderGraphPassType::Compute,
        [](RenderGraphBuilder& builder, SimplePassData& data)
        {
            data.colorTarget = builder.Write(RGTextureHandle{999}, RHIResourceState::RenderTarget);
        },
        [](const SimplePassData&, RHICommandContext&) {});

    graph.Compile();

    const auto& stats = graph.GetCompileStats();
    EXPECT_EQ(stats.totalPasses, 1u);
    EXPECT_FALSE(stats.compileValid);
    EXPECT_EQ(stats.invalidResourceUsageCount, 1u);
    EXPECT_EQ(stats.incompatibleStateUsageCount, 1u);
    EXPECT_EQ(stats.validationErrorCount, 2u);
    EXPECT_FALSE(stats.executionOrderFallbackUsed);
}
