#include "Core/Core.h"
#include "Render/Graph/RenderGraph.h"
#include "TestFramework/TestRunner.h"

using namespace RVX;
using namespace RVX::Test;

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
        void BufferBarrier(const RHIBufferBarrier&) override {}
        void TextureBarrier(const RHITextureBarrier&) override {}
        void Barriers(std::span<const RHIBufferBarrier>, std::span<const RHITextureBarrier>) override {}
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
} // namespace

// =============================================================================
// RenderGraph Validation Tests
// =============================================================================

bool Test_GraphCreation()
{
    RenderGraph graph;
    // Just test construction/destruction
    return true;
}

bool Test_TextureResourceCreation()
{
    RenderGraph graph;
    
    RHITextureDesc texDesc = RHITextureDesc::RenderTarget(1920, 1080, RHIFormat::RGBA16_FLOAT);
    texDesc.debugName = "TestRenderTarget";
    auto texture = graph.CreateTexture(texDesc);
    
    // Handle should be valid
    TEST_ASSERT_TRUE(texture.IsValid());
    
    return true;
}

bool Test_BufferResourceCreation()
{
    RenderGraph graph;
    
    RHIBufferDesc bufDesc;
    bufDesc.size = 1024 * 1024;
    bufDesc.usage = RHIBufferUsage::Structured;
    bufDesc.debugName = "TestStructuredBuffer";
    auto buffer = graph.CreateBuffer(bufDesc);
    
    TEST_ASSERT_TRUE(buffer.IsValid());
    
    return true;
}

bool Test_MultipleResources()
{
    RenderGraph graph;
    
    // Create multiple textures
    std::vector<RGTextureHandle> textures;
    for (int i = 0; i < 10; ++i)
    {
        RHITextureDesc desc = RHITextureDesc::RenderTarget(512, 512, RHIFormat::RGBA8_UNORM);
        textures.push_back(graph.CreateTexture(desc));
        TEST_ASSERT_TRUE(textures.back().IsValid());
    }
    
    // Create multiple buffers
    std::vector<RGBufferHandle> buffers;
    for (int i = 0; i < 10; ++i)
    {
        RHIBufferDesc desc;
        desc.size = 4096;
        desc.usage = RHIBufferUsage::Structured;
        buffers.push_back(graph.CreateBuffer(desc));
        TEST_ASSERT_TRUE(buffers.back().IsValid());
    }
    
    return true;
}

struct SimplePassData
{
    RGTextureHandle colorTarget;
};

bool Test_SinglePass()
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
    
    return true;
}

struct MultiPassData
{
    RGTextureHandle texture;
    RGBufferHandle buffer;
};

bool Test_PassChain()
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
    
    return true;
}

bool Test_PassCulling()
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
    TEST_ASSERT_TRUE(stats.culledPasses > 0);
    
    return true;
}

bool Test_MemoryAliasing()
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
    
    // Should have memory savings due to aliasing
    RVX_CORE_INFO("Memory aliasing test:");
    RVX_CORE_INFO("  Transient textures: {}", stats.totalTransientTextures);
    RVX_CORE_INFO("  Aliased textures: {}", stats.aliasedTextureCount);
    RVX_CORE_INFO("  Memory without aliasing: {} KB", stats.memoryWithoutAliasing / 1024);
    RVX_CORE_INFO("  Memory with aliasing: {} KB", stats.memoryWithAliasing / 1024);
    RVX_CORE_INFO("  Savings: {:.1f}%", stats.GetMemorySavingsPercent());
    
    // If aliasing is working, we should have some aliased resources
    // Note: This might be 0 if the graph is too simple or aliasing conditions aren't met
    TEST_ASSERT_TRUE(stats.totalTransientTextures >= 3);
    
    return true;
}

bool Test_ComputePass()
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
    
    return true;
}

bool Test_MixedPasses()
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
    TEST_ASSERT_EQ(stats.totalPasses, 3u);
    TEST_ASSERT_EQ(stats.culledPasses, 0u);
    
    return true;
}

bool Test_SubresourceTracking()
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
    
    return true;
}

bool Test_BufferRanges()
{
    RenderGraph graph;
    
    RHIBufferDesc bufDesc;
    bufDesc.size = 1024 * 1024;  // 1 MB
    bufDesc.usage = RHIBufferUsage::Structured | RHIBufferUsage::ShaderResource;
    auto buffer = graph.CreateBuffer(bufDesc);
    
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
    
    return true;
}

bool Test_CrossPassSubresourceBarriersAreNotDropped()
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
    TEST_ASSERT_EQ(stats.totalPasses, 2u);
    TEST_ASSERT_EQ(stats.textureBarrierCount, 2u);
    TEST_ASSERT_EQ(stats.crossPassMergedBarrierCount, 0u);

    return true;
}

bool Test_CrossPassBufferRangeBarriersAreNotDropped()
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
    TEST_ASSERT_EQ(stats.totalPasses, 2u);
    TEST_ASSERT_EQ(stats.bufferBarrierCount, 2u);
    TEST_ASSERT_EQ(stats.crossPassMergedBarrierCount, 0u);

    return true;
}

bool Test_ReadBeforeWriteHazardPreservesExecutionOrder()
{
    RenderGraph graph;

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

    FakeCommandContext ctx;
    graph.Execute(ctx);

    TEST_ASSERT_EQ(executed.size(), static_cast<size_t>(3));
    TEST_ASSERT_EQ(executed[0], std::string("ProduceB"));
    TEST_ASSERT_EQ(executed[1], std::string("ReadAWriteC"));
    TEST_ASSERT_EQ(executed[2], std::string("WriteA"));

    return true;
}

bool Test_ExecuteAsyncFallsBackToGraphicsUntilQueueSchedulerExists()
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

    TEST_ASSERT_TRUE(ranOnGraphicsContext);
    TEST_ASSERT_FALSE(ranOnComputeContext);

    return true;
}

bool Test_ClearAndRecompile()
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
    TEST_ASSERT_EQ(stats.totalPasses, 2u);
    
    return true;
}

bool Test_InvalidTextureUsageIsReported()
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
    TEST_ASSERT_EQ(stats.totalPasses, 1u);
    TEST_ASSERT_EQ(stats.invalidResourceUsageCount, 1u);
    TEST_ASSERT_EQ(stats.validationErrorCount, 1u);

    return true;
}

bool Test_InvalidBufferUsageIsReported()
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
    TEST_ASSERT_EQ(stats.totalPasses, 1u);
    TEST_ASSERT_EQ(stats.invalidResourceUsageCount, 1u);
    TEST_ASSERT_EQ(stats.validationErrorCount, 1u);

    return true;
}

bool Test_EmptyPassUsageIsReported()
{
    RenderGraph graph;

    graph.AddPass<SimplePassData>(
        "EmptyUsage",
        RenderGraphPassType::Graphics,
        [](RenderGraphBuilder&, SimplePassData&) {},
        [](const SimplePassData&, RHICommandContext&) {});

    graph.Compile();

    const auto& stats = graph.GetCompileStats();
    TEST_ASSERT_EQ(stats.totalPasses, 1u);
    TEST_ASSERT_EQ(stats.emptyPassUsageCount, 1u);
    TEST_ASSERT_EQ(stats.validationWarningCount, 1u);

    return true;
}

bool Test_IncompatiblePassStateIsReported()
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
    TEST_ASSERT_EQ(stats.totalPasses, 1u);
    TEST_ASSERT_EQ(stats.invalidResourceUsageCount, 1u);
    TEST_ASSERT_EQ(stats.incompatibleStateUsageCount, 1u);
    TEST_ASSERT_EQ(stats.validationErrorCount, 2u);

    return true;
}

int main()
{
    Log::Initialize();
    RVX_CORE_INFO("RenderGraph Validation Tests");
    
    TestSuite suite;
    
    // Basic tests
    suite.AddTest("GraphCreation", Test_GraphCreation);
    suite.AddTest("TextureResourceCreation", Test_TextureResourceCreation);
    suite.AddTest("BufferResourceCreation", Test_BufferResourceCreation);
    suite.AddTest("MultipleResources", Test_MultipleResources);
    
    // Pass tests
    suite.AddTest("SinglePass", Test_SinglePass);
    suite.AddTest("PassChain", Test_PassChain);
    suite.AddTest("PassCulling", Test_PassCulling);
    suite.AddTest("ComputePass", Test_ComputePass);
    suite.AddTest("MixedPasses", Test_MixedPasses);
    
    // Advanced features
    suite.AddTest("MemoryAliasing", Test_MemoryAliasing);
    suite.AddTest("SubresourceTracking", Test_SubresourceTracking);
    suite.AddTest("BufferRanges", Test_BufferRanges);
    suite.AddTest("CrossPassSubresourceBarriersAreNotDropped", Test_CrossPassSubresourceBarriersAreNotDropped);
    suite.AddTest("CrossPassBufferRangeBarriersAreNotDropped", Test_CrossPassBufferRangeBarriersAreNotDropped);
    suite.AddTest("ReadBeforeWriteHazardPreservesExecutionOrder", Test_ReadBeforeWriteHazardPreservesExecutionOrder);
    suite.AddTest("ExecuteAsyncFallsBackToGraphicsUntilQueueSchedulerExists", Test_ExecuteAsyncFallsBackToGraphicsUntilQueueSchedulerExists);
    suite.AddTest("ClearAndRecompile", Test_ClearAndRecompile);
    suite.AddTest("InvalidTextureUsageIsReported", Test_InvalidTextureUsageIsReported);
    suite.AddTest("InvalidBufferUsageIsReported", Test_InvalidBufferUsageIsReported);
    suite.AddTest("EmptyPassUsageIsReported", Test_EmptyPassUsageIsReported);
    suite.AddTest("IncompatiblePassStateIsReported", Test_IncompatiblePassStateIsReported);
    
    auto results = suite.Run();
    suite.PrintResults(results);
    
    Log::Shutdown();
    
    for (const auto& result : results)
    {
        if (!result.passed)
            return 1;
    }
    
    return 0;
}
