#include "Core/Core.h"
#include "Render/Graph/RenderGraph.h"
#include "Render/Graph/TransientResourcePool.h"
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace RVX;

namespace
{
    std::string ReadTextFile(const std::filesystem::path& path)
    {
        std::ifstream file(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    }

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
        void BeginEvent(const char* name, uint32 = 0) override
        {
            if (name)
            {
                events.push_back(name);
            }
        }
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
        void SignalFence(RHIFence*, uint64 value) override { signaledFenceValues.push_back(value); }
        void WaitFence(RHIFence*, uint64 value) override { waitedFenceValues.push_back(value); }

        std::vector<std::string> events;
        std::vector<RHIBufferBarrier> bufferBarriers;
        std::vector<RHITextureBarrier> textureBarriers;
        std::vector<uint64> signaledFenceValues;
        std::vector<uint64> waitedFenceValues;
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

        RHICapabilities& MutableCapabilities() { return m_capabilities; }

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

TEST(RenderGraphValidation, ExecuteDiagnosticsRecordsGraphicsQueueTimeline)
{
    RenderGraph graph;

    RHIBufferDesc bufferDesc;
    bufferDesc.size = 1024;
    bufferDesc.usage = RHIBufferUsage::Structured | RHIBufferUsage::UnorderedAccess;
    FakeBuffer buffer(bufferDesc);

    RGBufferHandle imported = graph.ImportBuffer(&buffer, RHIResourceState::Common);

    struct BufferPassData
    {
        RGBufferHandle buffer;
    };

    graph.AddPass<BufferPassData>(
        "GraphicsTimeline",
        RenderGraphPassType::Graphics,
        [imported](RenderGraphBuilder& builder, BufferPassData& data)
        {
            data.buffer = builder.Write(imported, RHIResourceState::UnorderedAccess);
        },
        [](const BufferPassData&, RHICommandContext&) {});

    graph.SetExportState(imported, RHIResourceState::ShaderResource);
    graph.Compile();
    ASSERT_TRUE(graph.GetCompileStats().compileValid);

    FakeCommandContext ctx;
    graph.Execute(ctx);

    const auto& stats = graph.GetCompileStats();
    EXPECT_EQ(stats.lastExecutedPassCount, 1u);

    RenderGraph::Diagnostics diagnostics = graph.GetDiagnostics();
    ASSERT_EQ(diagnostics.passes.size(), 1u);
    EXPECT_TRUE(diagnostics.passes[0].executedLastRun);
    EXPECT_EQ(diagnostics.passes[0].executionQueue, RenderGraph::DiagnosticExecutionQueue::Graphics);
    EXPECT_EQ(diagnostics.passes[0].executionSerial, 0u);

    std::string dump = graph.ExportDiagnosticsText();
    EXPECT_NE(dump.find("Last execution: passes=1"), std::string::npos);
    EXPECT_NE(dump.find("GraphicsTimeline type=Graphics culled=false executed=true queue=Graphics serial=0"), std::string::npos);
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

TEST(RenderGraphValidation, DiagnosticsSnapshotReportsPassResourcesLifetimesAndMemory)
{
    RenderGraph graph;

    RHITextureDesc colorDesc = RHITextureDesc::RenderTarget(64, 64, RHIFormat::RGBA8_UNORM);
    colorDesc.debugName = "DiagnosticColor";
    RGTextureHandle color = graph.CreateTexture(colorDesc);

    RHIBufferDesc bufferDesc;
    bufferDesc.size = 1024;
    bufferDesc.usage = RHIBufferUsage::Structured | RHIBufferUsage::UnorderedAccess;
    bufferDesc.debugName = "DiagnosticBuffer";
    RGBufferHandle buffer = graph.CreateBuffer(bufferDesc);

    struct ProduceColorData
    {
        RGTextureHandle output;
    };

    graph.AddPass<ProduceColorData>(
        "ProduceColor",
        RenderGraphPassType::Graphics,
        [color](RenderGraphBuilder& builder, ProduceColorData& data)
        {
            data.output = builder.Write(color, RHIResourceState::RenderTarget);
        },
        [](const ProduceColorData&, RHICommandContext&)
        {
        });

    struct ConsumeColorData
    {
        RGTextureHandle input;
        RGBufferHandle output;
    };

    graph.AddPass<ConsumeColorData>(
        "ConsumeColor",
        RenderGraphPassType::Compute,
        [color, buffer](RenderGraphBuilder& builder, ConsumeColorData& data)
        {
            data.input = builder.Read(color, RHIResourceState::ShaderResource, RHIShaderStage::Compute);
            data.output = builder.Write(buffer, RHIResourceState::UnorderedAccess);
        },
        [](const ConsumeColorData&, RHICommandContext&)
        {
        });

    graph.SetExportState(buffer, RHIResourceState::ShaderResource);
    graph.Compile();

    auto diagnostics = graph.GetDiagnostics();
    EXPECT_STREQ(diagnostics.schemaId, RVX_RENDER_GRAPH_DIAGNOSTICS_SCHEMA_ID);
    EXPECT_EQ(diagnostics.schemaVersion, RVX_RENDER_GRAPH_DIAGNOSTICS_SCHEMA_VERSION);
    ASSERT_TRUE(diagnostics.compileStats.compileValid);
    ASSERT_EQ(diagnostics.passes.size(), 2u);
    ASSERT_EQ(diagnostics.resources.size(), 2u);
    ASSERT_EQ(diagnostics.executionOrder.size(), 2u);

    EXPECT_EQ(diagnostics.passes[0].name, "ProduceColor");
    EXPECT_EQ(diagnostics.passes[0].type, RenderGraphPassType::Graphics);
    EXPECT_FALSE(diagnostics.passes[0].culled);
    ASSERT_EQ(diagnostics.passes[0].usages.size(), 1u);
    EXPECT_EQ(diagnostics.passes[0].usages[0].access, RenderGraph::DiagnosticAccessType::Write);
    EXPECT_EQ(diagnostics.passes[0].usages[0].type, RenderGraph::DiagnosticResourceType::Texture);

    EXPECT_EQ(diagnostics.passes[1].name, "ConsumeColor");
    EXPECT_EQ(diagnostics.passes[1].type, RenderGraphPassType::Compute);
    ASSERT_EQ(diagnostics.passes[1].usages.size(), 2u);
    EXPECT_EQ(diagnostics.passes[1].usages[0].access, RenderGraph::DiagnosticAccessType::Read);
    EXPECT_EQ(diagnostics.passes[1].usages[0].stages, RHIShaderStage::Compute);
    EXPECT_EQ(diagnostics.passes[1].usages[1].access, RenderGraph::DiagnosticAccessType::Write);

    auto textureIt = std::find_if(
        diagnostics.resources.begin(),
        diagnostics.resources.end(),
        [](const RenderGraph::ResourceDiagnostic& resource)
        {
            return resource.type == RenderGraph::DiagnosticResourceType::Texture;
        });
    ASSERT_NE(textureIt, diagnostics.resources.end());
    EXPECT_EQ(textureIt->name, "DiagnosticColor");
    EXPECT_TRUE(textureIt->used);
    EXPECT_EQ(textureIt->firstUsePass, 0u);
    EXPECT_EQ(textureIt->lastUsePass, 1u);
    EXPECT_EQ(textureIt->width, 64u);
    EXPECT_EQ(textureIt->height, 64u);
    EXPECT_GE(textureIt->estimatedMemoryBytes, 64u * 64u * 4u);

    auto bufferIt = std::find_if(
        diagnostics.resources.begin(),
        diagnostics.resources.end(),
        [](const RenderGraph::ResourceDiagnostic& resource)
        {
            return resource.type == RenderGraph::DiagnosticResourceType::Buffer;
        });
    ASSERT_NE(bufferIt, diagnostics.resources.end());
    EXPECT_EQ(bufferIt->name, "DiagnosticBuffer");
    EXPECT_TRUE(bufferIt->used);
    EXPECT_TRUE(bufferIt->hasExportState);
    EXPECT_EQ(bufferIt->exportState, RHIResourceState::ShaderResource);
    EXPECT_EQ(bufferIt->bufferSize, 1024u);
    EXPECT_GE(bufferIt->estimatedMemoryBytes, 1024u);

    EXPECT_GE(diagnostics.estimatedTransientMemoryBytes,
              textureIt->estimatedMemoryBytes + bufferIt->estimatedMemoryBytes);
    EXPECT_EQ(diagnostics.estimatedUsedTransientMemoryBytes,
              diagnostics.estimatedTransientMemoryBytes);

    std::string dump = graph.ExportDiagnosticsText();
    EXPECT_NE(dump.find("RenderGraph Diagnostics"), std::string::npos);
    EXPECT_NE(dump.find("Schema: 3"), std::string::npos);
    EXPECT_NE(dump.find(RVX_RENDER_GRAPH_DIAGNOSTICS_SCHEMA_ID), std::string::npos);
    EXPECT_NE(dump.find("ProduceColor"), std::string::npos);
    EXPECT_NE(dump.find("DiagnosticColor"), std::string::npos);
    EXPECT_NE(dump.find("Estimated transient memory"), std::string::npos);

    std::string json = graph.ExportDiagnosticsJson();
    EXPECT_NE(json.find("\"schemaVersion\": 3"), std::string::npos);
    EXPECT_NE(json.find("\"schemaId\": \"RVX.RenderGraph.Diagnostics\""), std::string::npos);
    EXPECT_NE(json.find("\"id\": \"renderGraphDiagnosticsJson\""), std::string::npos);
    EXPECT_NE(json.find("\"kind\": \"RenderGraphDiagnosticsJson\""), std::string::npos);
    EXPECT_NE(json.find("\"contentType\": \"application/json\""), std::string::npos);
    EXPECT_NE(json.find("\"contentHash\": \"\""), std::string::npos);
    EXPECT_NE(json.find("\"relativePath\": \"\""), std::string::npos);
    EXPECT_NE(json.find("\"compileStats\": {"), std::string::npos);
    EXPECT_NE(json.find("\"memory\": {"), std::string::npos);
    EXPECT_NE(json.find("\"schedule\": {"), std::string::npos);
    EXPECT_NE(json.find("\"executionOrder\": [0, 1]"), std::string::npos);
    EXPECT_NE(json.find("\"passes\": ["), std::string::npos);
    EXPECT_NE(json.find("\"name\": \"ProduceColor\""), std::string::npos);
    EXPECT_NE(json.find("\"type\": \"Compute\""), std::string::npos);
    EXPECT_NE(json.find("\"usages\": ["), std::string::npos);
    EXPECT_NE(json.find("\"resources\": ["), std::string::npos);
    EXPECT_NE(json.find("\"name\": \"DiagnosticColor\""), std::string::npos);
    EXPECT_NE(json.find("\"plannedQueueBatches\": ["), std::string::npos);
    EXPECT_NE(json.find("\"queueSyncs\": ["), std::string::npos);
    EXPECT_FALSE(graph.SaveDiagnosticsJson(nullptr));
    EXPECT_FALSE(graph.SaveDiagnosticsJson(""));

    const std::string suffix =
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    const std::filesystem::path jsonPath =
        std::filesystem::temp_directory_path() / ("RVX_RenderGraphDiagnostics_" + suffix + ".json");
    const std::string jsonPathString = jsonPath.string();
    ASSERT_TRUE(graph.SaveDiagnosticsJson(jsonPathString.c_str()));
    EXPECT_EQ(ReadTextFile(jsonPath), json);
    std::error_code removeError;
    std::filesystem::remove(jsonPath, removeError);

    graph.Clear();
    diagnostics = graph.GetDiagnostics();
    EXPECT_STREQ(diagnostics.schemaId, RVX_RENDER_GRAPH_DIAGNOSTICS_SCHEMA_ID);
    EXPECT_EQ(diagnostics.schemaVersion, RVX_RENDER_GRAPH_DIAGNOSTICS_SCHEMA_VERSION);
    EXPECT_TRUE(diagnostics.passes.empty());
    EXPECT_TRUE(diagnostics.resources.empty());
    EXPECT_TRUE(diagnostics.executionOrder.empty());
    EXPECT_EQ(diagnostics.estimatedTransientMemoryBytes, 0u);
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

TEST(RenderGraphValidation, ExecuteAsyncFallsBackToGraphicsWhenBackendDoesNotSupportQueueSync)
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
    EXPECT_EQ(stats.asyncFallbackReason, RenderGraph::AsyncComputeFallbackReason::BackendUnsupported);
    EXPECT_EQ(stats.asyncComputeEligiblePasses, 1u);
    EXPECT_EQ(stats.asyncComputeScheduledPasses, 0u);
    EXPECT_EQ(stats.lastExecutedPassCount, 1u);
    EXPECT_TRUE(ranOnGraphicsContext);
    EXPECT_FALSE(ranOnComputeContext);

    RenderGraph::Diagnostics diagnostics = graph.GetDiagnostics();
    ASSERT_EQ(diagnostics.passes.size(), 1u);
    EXPECT_TRUE(diagnostics.passes[0].executedLastRun);
    EXPECT_EQ(diagnostics.passes[0].executionQueue, RenderGraph::DiagnosticExecutionQueue::Graphics);
    EXPECT_EQ(diagnostics.passes[0].executionSerial, 0u);
}

TEST(RenderGraphValidation, ExecuteAsyncSchedulesComputePassesWhenQueueSyncIsSupported)
{
    FakeDevice device;
    device.MutableCapabilities().supportsAsyncCompute = true;
    device.MutableCapabilities().supportsExplicitQueueFenceSignal = true;
    device.MutableCapabilities().supportsQueueFenceWait = true;

    RenderGraph graph;
    graph.SetDevice(&device);

    RHIBufferDesc bufferDesc;
    bufferDesc.size = 1024;
    bufferDesc.usage = RHIBufferUsage::Structured | RHIBufferUsage::UnorderedAccess;
    FakeBuffer buffer(bufferDesc);

    RGBufferHandle imported = graph.ImportBuffer(&buffer, RHIResourceState::Common);

    FakeCommandContext graphicsCtx;
    FakeCommandContext computeCtx;
    FakeFence computeFence;
    bool graphicsPassRanOnGraphicsContext = false;
    bool computePassRanOnComputeContext = false;

    struct BufferPassData
    {
        RGBufferHandle buffer;
    };

    graph.AddPass<BufferPassData>(
        "GraphicsProduce",
        RenderGraphPassType::Graphics,
        [imported](RenderGraphBuilder& builder, BufferPassData& data)
        {
            data.buffer = builder.Write(imported, RHIResourceState::UnorderedAccess);
        },
        [&](const BufferPassData&, RHICommandContext& ctx)
        {
            graphicsPassRanOnGraphicsContext = (&ctx == &graphicsCtx);
        });

    graph.AddPass<BufferPassData>(
        "ComputeConsume",
        RenderGraphPassType::Compute,
        [imported](RenderGraphBuilder& builder, BufferPassData& data)
        {
            data.buffer = builder.ReadWrite(imported);
        },
        [&](const BufferPassData&, RHICommandContext& ctx)
        {
            computePassRanOnComputeContext = (&ctx == &computeCtx);
        });

    graph.SetExportState(imported, RHIResourceState::ShaderResource);
    graph.Compile();
    ASSERT_TRUE(graph.GetCompileStats().compileValid);

    graph.ExecuteAsync(graphicsCtx, &computeCtx, &computeFence, 7);

    const auto& stats = graph.GetCompileStats();
    EXPECT_TRUE(stats.asyncComputeSupported);
    EXPECT_FALSE(stats.asyncFallbackUsed);
    EXPECT_EQ(stats.asyncFallbackReason, RenderGraph::AsyncComputeFallbackReason::None);
    EXPECT_EQ(stats.asyncComputeEligiblePasses, 1u);
    EXPECT_EQ(stats.asyncComputeScheduledPasses, 1u);
    EXPECT_EQ(stats.asyncGraphicsScheduledPasses, 1u);
    EXPECT_EQ(stats.asyncFenceSignalCount, 2u);
    EXPECT_EQ(stats.asyncFenceWaitCount, 2u);
    EXPECT_EQ(stats.asyncCrossQueueDependencyCount, 1u);
    EXPECT_EQ(stats.asyncFinalQueueJoinCount, 1u);
    EXPECT_EQ(stats.lastExecutedPassCount, 2u);

    EXPECT_TRUE(graphicsPassRanOnGraphicsContext);
    EXPECT_TRUE(computePassRanOnComputeContext);

    EXPECT_EQ(graphicsCtx.events.size(), 1u);
    EXPECT_EQ(graphicsCtx.events[0], "GraphicsProduce");
    EXPECT_EQ(computeCtx.events.size(), 1u);
    EXPECT_EQ(computeCtx.events[0], "ComputeConsume");

    EXPECT_EQ(graphicsCtx.signaledFenceValues.size(), 1u);
    EXPECT_EQ(graphicsCtx.waitedFenceValues.size(), 1u);
    EXPECT_EQ(computeCtx.signaledFenceValues.size(), 1u);
    EXPECT_EQ(computeCtx.waitedFenceValues.size(), 1u);
    EXPECT_EQ(graphicsCtx.signaledFenceValues[0], computeCtx.waitedFenceValues[0]);
    EXPECT_EQ(computeCtx.signaledFenceValues[0], graphicsCtx.waitedFenceValues[0]);
    EXPECT_GT(computeCtx.signaledFenceValues[0], graphicsCtx.signaledFenceValues[0]);

    RenderGraph::Diagnostics diagnostics = graph.GetDiagnostics();
    ASSERT_EQ(diagnostics.passes.size(), 2u);
    EXPECT_TRUE(diagnostics.passes[0].executedLastRun);
    EXPECT_TRUE(diagnostics.passes[1].executedLastRun);
    EXPECT_EQ(diagnostics.passes[0].executionQueue, RenderGraph::DiagnosticExecutionQueue::Graphics);
    EXPECT_EQ(diagnostics.passes[1].executionQueue, RenderGraph::DiagnosticExecutionQueue::Compute);
    EXPECT_EQ(diagnostics.passes[0].executionSerial, 0u);
    EXPECT_EQ(diagnostics.passes[1].executionSerial, 1u);
    ASSERT_EQ(diagnostics.queueBatches.size(), 2u);
    EXPECT_EQ(diagnostics.actualQueueBatchCount, 2u);
    EXPECT_EQ(diagnostics.actualQueueSwitchCount, 1u);
    EXPECT_EQ(diagnostics.actualQueueSyncCount, 2u);
    EXPECT_EQ(diagnostics.actualCrossQueueSyncCount, 1u);
    EXPECT_EQ(diagnostics.queueBatches[0].queue, RenderGraph::DiagnosticExecutionQueue::Graphics);
    EXPECT_EQ(diagnostics.queueBatches[0].passIndices, std::vector<uint32>({0u}));
    EXPECT_EQ(diagnostics.queueBatches[1].queue, RenderGraph::DiagnosticExecutionQueue::Compute);
    EXPECT_EQ(diagnostics.queueBatches[1].passIndices, std::vector<uint32>({1u}));
    ASSERT_EQ(diagnostics.queueSyncs.size(), 2u);
    EXPECT_EQ(diagnostics.queueSyncs[0].sourceQueue, RenderGraph::DiagnosticExecutionQueue::Graphics);
    EXPECT_EQ(diagnostics.queueSyncs[0].targetQueue, RenderGraph::DiagnosticExecutionQueue::Compute);
    EXPECT_EQ(diagnostics.queueSyncs[0].reason, RenderGraph::DiagnosticSyncReason::CrossQueueDependency);
    EXPECT_EQ(diagnostics.queueSyncs[0].sourcePassIndex, 0u);
    EXPECT_EQ(diagnostics.queueSyncs[0].targetPassIndex, 1u);
    EXPECT_EQ(diagnostics.queueSyncs[1].sourceQueue, RenderGraph::DiagnosticExecutionQueue::Compute);
    EXPECT_EQ(diagnostics.queueSyncs[1].targetQueue, RenderGraph::DiagnosticExecutionQueue::Graphics);
    EXPECT_EQ(diagnostics.queueSyncs[1].reason, RenderGraph::DiagnosticSyncReason::FinalQueueJoin);
    EXPECT_EQ(diagnostics.queueSyncs[1].sourcePassIndex, 1u);
    EXPECT_EQ(diagnostics.queueSyncs[1].targetPassIndex, RVX_INVALID_INDEX);

    std::string dump = graph.ExportDiagnosticsText();
    EXPECT_NE(dump.find("Async compute"), std::string::npos);
    EXPECT_NE(dump.find("scheduled=1"), std::string::npos);
    EXPECT_NE(dump.find("fenceSignals=2"), std::string::npos);
    EXPECT_NE(dump.find("crossQueueDeps=1"), std::string::npos);
    EXPECT_NE(dump.find("finalJoins=1"), std::string::npos);
    EXPECT_NE(dump.find("dependencies=[0]"), std::string::npos);
    EXPECT_NE(dump.find("Queue Batches"), std::string::npos);
    EXPECT_NE(dump.find("Queue Syncs"), std::string::npos);
    EXPECT_NE(dump.find("Schedule efficiency"), std::string::npos);
    EXPECT_NE(dump.find("actualBatches=2"), std::string::npos);
    EXPECT_NE(dump.find("actualCrossQueueSyncs=1"), std::string::npos);
    EXPECT_NE(dump.find("Graphics -> Compute reason=CrossQueueDependency"), std::string::npos);
    EXPECT_NE(dump.find("Compute -> Graphics reason=FinalQueueJoin"), std::string::npos);
    EXPECT_NE(dump.find("ComputeConsume type=Compute culled=false executed=true queue=Compute serial=1"), std::string::npos);
}

TEST(RenderGraphValidation, DiagnosticsReportsQueueBatchesAndSyncPoints)
{
    FakeDevice device;
    device.MutableCapabilities().supportsAsyncCompute = true;
    device.MutableCapabilities().supportsExplicitQueueFenceSignal = true;
    device.MutableCapabilities().supportsQueueFenceWait = true;

    RenderGraph graph;
    graph.SetDevice(&device);

    RHIBufferDesc bufferDesc;
    bufferDesc.size = 1024;
    bufferDesc.usage = RHIBufferUsage::Structured | RHIBufferUsage::UnorderedAccess;
    FakeBuffer buffer(bufferDesc);

    RGBufferHandle imported = graph.ImportBuffer(&buffer, RHIResourceState::Common);
    FakeCommandContext graphicsCtx;
    FakeCommandContext computeCtx;
    FakeFence computeFence;

    struct BufferPassData
    {
        RGBufferHandle buffer;
    };

    graph.AddPass<BufferPassData>(
        "ScheduleGraphics",
        RenderGraphPassType::Graphics,
        [imported](RenderGraphBuilder& builder, BufferPassData& data)
        {
            data.buffer = builder.Write(imported, RHIResourceState::UnorderedAccess);
        },
        [](const BufferPassData&, RHICommandContext&) {});

    graph.AddPass<BufferPassData>(
        "ScheduleCompute",
        RenderGraphPassType::Compute,
        [imported](RenderGraphBuilder& builder, BufferPassData& data)
        {
            data.buffer = builder.ReadWrite(imported);
        },
        [](const BufferPassData&, RHICommandContext&) {});

    graph.SetExportState(imported, RHIResourceState::ShaderResource);
    graph.Compile();
    ASSERT_TRUE(graph.GetCompileStats().compileValid);

    graph.ExecuteAsync(graphicsCtx, &computeCtx, &computeFence, 3);

    RenderGraph::Diagnostics diagnostics = graph.GetDiagnostics();
    ASSERT_EQ(diagnostics.queueBatches.size(), 2u);
    EXPECT_EQ(diagnostics.actualQueueBatchCount, 2u);
    EXPECT_EQ(diagnostics.actualQueueSwitchCount, 1u);
    EXPECT_EQ(diagnostics.actualQueueSyncCount, 2u);
    EXPECT_EQ(diagnostics.actualCrossQueueSyncCount, 1u);
    EXPECT_EQ(diagnostics.actualMatchedPlannedSyncCount, 1u);
    EXPECT_EQ(diagnostics.actualUnplannedQueueSyncCount, 1u);
    EXPECT_EQ(diagnostics.actualConservativeFinalJoinCount, 1u);
    EXPECT_EQ(diagnostics.queueBatches[0].batchIndex, 0u);
    EXPECT_EQ(diagnostics.queueBatches[0].queue, RenderGraph::DiagnosticExecutionQueue::Graphics);
    EXPECT_EQ(diagnostics.queueBatches[0].firstExecutionSerial, 0u);
    EXPECT_EQ(diagnostics.queueBatches[0].lastExecutionSerial, 0u);
    EXPECT_EQ(diagnostics.queueBatches[0].passIndices, std::vector<uint32>({0u}));
    EXPECT_EQ(diagnostics.queueBatches[1].batchIndex, 1u);
    EXPECT_EQ(diagnostics.queueBatches[1].queue, RenderGraph::DiagnosticExecutionQueue::Compute);
    EXPECT_EQ(diagnostics.queueBatches[1].firstExecutionSerial, 1u);
    EXPECT_EQ(diagnostics.queueBatches[1].lastExecutionSerial, 1u);
    EXPECT_EQ(diagnostics.queueBatches[1].passIndices, std::vector<uint32>({1u}));

    ASSERT_EQ(diagnostics.queueSyncs.size(), 2u);
    EXPECT_EQ(diagnostics.queueSyncs[0].sourceQueue, RenderGraph::DiagnosticExecutionQueue::Graphics);
    EXPECT_EQ(diagnostics.queueSyncs[0].targetQueue, RenderGraph::DiagnosticExecutionQueue::Compute);
    EXPECT_EQ(diagnostics.queueSyncs[0].reason, RenderGraph::DiagnosticSyncReason::CrossQueueDependency);
    EXPECT_EQ(diagnostics.queueSyncs[0].sourcePassIndex, 0u);
    EXPECT_EQ(diagnostics.queueSyncs[0].targetPassIndex, 1u);
    EXPECT_TRUE(diagnostics.queueSyncs[0].coversPlannedSync);
    EXPECT_EQ(diagnostics.queueSyncs[0].plannedSyncIndex, 0u);
    EXPECT_EQ(diagnostics.queueSyncs[1].sourceQueue, RenderGraph::DiagnosticExecutionQueue::Compute);
    EXPECT_EQ(diagnostics.queueSyncs[1].targetQueue, RenderGraph::DiagnosticExecutionQueue::Graphics);
    EXPECT_EQ(diagnostics.queueSyncs[1].reason, RenderGraph::DiagnosticSyncReason::FinalQueueJoin);
    EXPECT_EQ(diagnostics.queueSyncs[1].sourcePassIndex, 1u);
    EXPECT_EQ(diagnostics.queueSyncs[1].targetPassIndex, RVX_INVALID_INDEX);
    EXPECT_FALSE(diagnostics.queueSyncs[1].coversPlannedSync);
    EXPECT_EQ(diagnostics.queueSyncs[1].plannedSyncIndex, RVX_INVALID_INDEX);
    EXPECT_LT(diagnostics.queueSyncs[0].fenceValue, diagnostics.queueSyncs[1].fenceValue);

    std::string dump = graph.ExportDiagnosticsText();
    EXPECT_NE(dump.find("Queue Batches"), std::string::npos);
    EXPECT_NE(dump.find("Queue Syncs"), std::string::npos);
    EXPECT_NE(dump.find("[0] queue=Graphics serial=[0,0] passes=[0]"), std::string::npos);
    EXPECT_NE(dump.find("[1] queue=Compute serial=[1,1] passes=[1]"), std::string::npos);
    EXPECT_NE(dump.find("Graphics -> Compute reason=CrossQueueDependency"), std::string::npos);
    EXPECT_NE(dump.find("Compute -> Graphics reason=FinalQueueJoin"), std::string::npos);
    EXPECT_NE(dump.find("actualMatchedPlannedSyncs=1"), std::string::npos);
    EXPECT_NE(dump.find("actualConservativeFinalJoins=1"), std::string::npos);
}

TEST(RenderGraphValidation, DiagnosticsReportsReadyListPlannedQueueBatches)
{
    FakeDevice device;
    device.MutableCapabilities().supportsAsyncCompute = true;
    device.MutableCapabilities().supportsExplicitQueueFenceSignal = true;
    device.MutableCapabilities().supportsQueueFenceWait = true;

    RenderGraph graph;
    graph.SetDevice(&device);

    RHIBufferDesc graphicsBufferDesc;
    graphicsBufferDesc.size = 1024;
    graphicsBufferDesc.usage = RHIBufferUsage::Structured | RHIBufferUsage::UnorderedAccess;
    FakeBuffer graphicsBuffer(graphicsBufferDesc);

    RHIBufferDesc computeBufferDesc;
    computeBufferDesc.size = 1024;
    computeBufferDesc.usage = RHIBufferUsage::Structured | RHIBufferUsage::UnorderedAccess;
    FakeBuffer computeBuffer(computeBufferDesc);

    RGBufferHandle graphicsResource = graph.ImportBuffer(&graphicsBuffer, RHIResourceState::Common);
    RGBufferHandle computeResource = graph.ImportBuffer(&computeBuffer, RHIResourceState::Common);

    struct BufferPassData
    {
        RGBufferHandle buffer;
    };

    graph.AddPass<BufferPassData>(
        "PlanGraphicsProduce",
        RenderGraphPassType::Graphics,
        [graphicsResource](RenderGraphBuilder& builder, BufferPassData& data)
        {
            data.buffer = builder.Write(graphicsResource, RHIResourceState::UnorderedAccess);
        },
        [](const BufferPassData&, RHICommandContext&) {});

    graph.AddPass<BufferPassData>(
        "PlanComputeIndependent",
        RenderGraphPassType::Compute,
        [computeResource](RenderGraphBuilder& builder, BufferPassData& data)
        {
            data.buffer = builder.Write(computeResource, RHIResourceState::UnorderedAccess);
        },
        [](const BufferPassData&, RHICommandContext&) {});

    graph.AddPass<BufferPassData>(
        "PlanGraphicsConsume",
        RenderGraphPassType::Graphics,
        [graphicsResource](RenderGraphBuilder& builder, BufferPassData& data)
        {
            data.buffer = builder.ReadWrite(graphicsResource);
        },
        [](const BufferPassData&, RHICommandContext&) {});

    graph.SetExportState(graphicsResource, RHIResourceState::ShaderResource);
    graph.SetExportState(computeResource, RHIResourceState::ShaderResource);
    graph.Compile();
    ASSERT_TRUE(graph.GetCompileStats().compileValid);

    RenderGraph::Diagnostics diagnostics = graph.GetDiagnostics();
    ASSERT_EQ(diagnostics.plannedQueueBatches.size(), 3u);
    EXPECT_EQ(diagnostics.plannedQueueBatchCount, 3u);
    EXPECT_EQ(diagnostics.plannedDependencyLevelCount, 2u);
    EXPECT_EQ(diagnostics.plannedComputeBatchCount, 1u);
    EXPECT_EQ(diagnostics.plannedAsyncOverlapCandidateLevelCount, 1u);
    EXPECT_EQ(diagnostics.plannedQueueSyncCount, 0u);
    EXPECT_EQ(diagnostics.plannedCrossQueueSyncCount, 0u);
    EXPECT_TRUE(diagnostics.plannedQueueSyncs.empty());
    EXPECT_EQ(diagnostics.plannedQueueBatches[0].dependencyLevel, 0u);
    EXPECT_EQ(diagnostics.plannedQueueBatches[0].queue, RenderGraph::DiagnosticExecutionQueue::Graphics);
    EXPECT_EQ(diagnostics.plannedQueueBatches[0].passIndices, std::vector<uint32>({0u}));
    EXPECT_EQ(diagnostics.plannedQueueBatches[1].dependencyLevel, 0u);
    EXPECT_EQ(diagnostics.plannedQueueBatches[1].queue, RenderGraph::DiagnosticExecutionQueue::Compute);
    EXPECT_EQ(diagnostics.plannedQueueBatches[1].passIndices, std::vector<uint32>({1u}));
    EXPECT_EQ(diagnostics.plannedQueueBatches[2].dependencyLevel, 1u);
    EXPECT_EQ(diagnostics.plannedQueueBatches[2].queue, RenderGraph::DiagnosticExecutionQueue::Graphics);
    EXPECT_EQ(diagnostics.plannedQueueBatches[2].passIndices, std::vector<uint32>({2u}));

    EXPECT_TRUE(diagnostics.queueBatches.empty());
    EXPECT_TRUE(diagnostics.queueSyncs.empty());

    std::string dump = graph.ExportDiagnosticsText();
    EXPECT_NE(dump.find("Planned Queue Batches"), std::string::npos);
    EXPECT_NE(dump.find("plannedBatches=3"), std::string::npos);
    EXPECT_NE(dump.find("plannedOverlapLevels=1"), std::string::npos);
    EXPECT_NE(dump.find("plannedSyncs=0"), std::string::npos);
    EXPECT_NE(dump.find("[0] level=0 queue=Graphics passes=[0]"), std::string::npos);
    EXPECT_NE(dump.find("[1] level=0 queue=Compute passes=[1]"), std::string::npos);
    EXPECT_NE(dump.find("[2] level=1 queue=Graphics passes=[2]"), std::string::npos);
}

TEST(RenderGraphValidation, DiagnosticsReportsPlannedSubmissionSyncGraph)
{
    RenderGraph graph;

    RHIBufferDesc bufferDesc;
    bufferDesc.size = 1024;
    bufferDesc.usage = RHIBufferUsage::Structured | RHIBufferUsage::UnorderedAccess;
    FakeBuffer buffer(bufferDesc);

    RGBufferHandle imported = graph.ImportBuffer(&buffer, RHIResourceState::Common);

    struct BufferPassData
    {
        RGBufferHandle buffer;
    };

    graph.AddPass<BufferPassData>(
        "PlannedSyncGraphicsProduce",
        RenderGraphPassType::Graphics,
        [imported](RenderGraphBuilder& builder, BufferPassData& data)
        {
            data.buffer = builder.Write(imported, RHIResourceState::UnorderedAccess);
        },
        [](const BufferPassData&, RHICommandContext&) {});

    graph.AddPass<BufferPassData>(
        "PlannedSyncComputeConsume",
        RenderGraphPassType::Compute,
        [imported](RenderGraphBuilder& builder, BufferPassData& data)
        {
            data.buffer = builder.ReadWrite(imported);
        },
        [](const BufferPassData&, RHICommandContext&) {});

    graph.SetExportState(imported, RHIResourceState::ShaderResource);
    graph.Compile();
    ASSERT_TRUE(graph.GetCompileStats().compileValid);

    RenderGraph::Diagnostics diagnostics = graph.GetDiagnostics();
    ASSERT_EQ(diagnostics.plannedQueueBatches.size(), 2u);
    EXPECT_EQ(diagnostics.plannedQueueBatches[0].queue, RenderGraph::DiagnosticExecutionQueue::Graphics);
    EXPECT_EQ(diagnostics.plannedQueueBatches[0].passIndices, std::vector<uint32>({0u}));
    EXPECT_TRUE(diagnostics.plannedQueueBatches[0].prerequisiteSyncIndices.empty());
    EXPECT_EQ(diagnostics.plannedQueueBatches[1].queue, RenderGraph::DiagnosticExecutionQueue::Compute);
    EXPECT_EQ(diagnostics.plannedQueueBatches[1].passIndices, std::vector<uint32>({1u}));
    EXPECT_EQ(diagnostics.plannedQueueBatches[1].prerequisiteSyncIndices, std::vector<uint32>({0u}));

    ASSERT_EQ(diagnostics.plannedQueueSyncs.size(), 1u);
    EXPECT_EQ(diagnostics.plannedQueueSyncCount, 1u);
    EXPECT_EQ(diagnostics.plannedCrossQueueSyncCount, 1u);
    EXPECT_EQ(diagnostics.plannedQueueSyncCoveredCount, 0u);
    EXPECT_EQ(diagnostics.plannedQueueSyncUncoveredCount, 1u);
    EXPECT_EQ(diagnostics.plannedQueueSyncs[0].syncIndex, 0u);
    EXPECT_EQ(diagnostics.plannedQueueSyncs[0].sourceBatchIndex, 0u);
    EXPECT_EQ(diagnostics.plannedQueueSyncs[0].targetBatchIndex, 1u);
    EXPECT_EQ(diagnostics.plannedQueueSyncs[0].sourceQueue, RenderGraph::DiagnosticExecutionQueue::Graphics);
    EXPECT_EQ(diagnostics.plannedQueueSyncs[0].targetQueue, RenderGraph::DiagnosticExecutionQueue::Compute);
    EXPECT_EQ(diagnostics.plannedQueueSyncs[0].reason, RenderGraph::DiagnosticSyncReason::CrossQueueDependency);
    EXPECT_EQ(diagnostics.plannedQueueSyncs[0].sourcePassIndex, 0u);
    EXPECT_EQ(diagnostics.plannedQueueSyncs[0].targetPassIndex, 1u);
    EXPECT_FALSE(diagnostics.plannedQueueSyncs[0].coveredByActualSync);
    EXPECT_EQ(diagnostics.plannedQueueSyncs[0].actualSyncIndex, RVX_INVALID_INDEX);
    EXPECT_EQ(diagnostics.actualQueueSyncCount, 0u);

    std::string dump = graph.ExportDiagnosticsText();
    EXPECT_NE(dump.find("Planned Queue Syncs"), std::string::npos);
    EXPECT_NE(dump.find("plannedSyncs=1"), std::string::npos);
    EXPECT_NE(dump.find("plannedCrossQueueSyncs=1"), std::string::npos);
    EXPECT_NE(dump.find("plannedUncoveredSyncs=1"), std::string::npos);
    EXPECT_NE(dump.find("[1] level=1 queue=Compute passes=[1] prerequisites=[0]"), std::string::npos);
    EXPECT_NE(dump.find("batch0 Graphics -> batch1 Compute reason=CrossQueueDependency"), std::string::npos);
}

TEST(RenderGraphValidation, SubmissionPlanExposesReusableReadyListPlan)
{
    RenderGraph graph;

    RHIBufferDesc bufferDesc;
    bufferDesc.size = 1024;
    bufferDesc.usage = RHIBufferUsage::Structured | RHIBufferUsage::UnorderedAccess;
    FakeBuffer buffer(bufferDesc);

    RGBufferHandle imported = graph.ImportBuffer(&buffer, RHIResourceState::Common);

    struct BufferPassData
    {
        RGBufferHandle buffer;
    };

    graph.AddPass<BufferPassData>(
        "PlanApiGraphicsProduce",
        RenderGraphPassType::Graphics,
        [imported](RenderGraphBuilder& builder, BufferPassData& data)
        {
            data.buffer = builder.Write(imported, RHIResourceState::UnorderedAccess);
        },
        [](const BufferPassData&, RHICommandContext&) {});

    graph.AddPass<BufferPassData>(
        "PlanApiComputeConsume",
        RenderGraphPassType::Compute,
        [imported](RenderGraphBuilder& builder, BufferPassData& data)
        {
            data.buffer = builder.ReadWrite(imported);
        },
        [](const BufferPassData&, RHICommandContext&) {});

    graph.SetExportState(imported, RHIResourceState::ShaderResource);
    graph.Compile();
    ASSERT_TRUE(graph.GetCompileStats().compileValid);

    RenderGraph::SubmissionPlan plan = graph.GetSubmissionPlan();
    ASSERT_EQ(plan.queueBatches.size(), 2u);
    EXPECT_EQ(plan.queueBatchCount, 2u);
    EXPECT_EQ(plan.dependencyLevelCount, 2u);
    EXPECT_EQ(plan.computeBatchCount, 1u);
    EXPECT_EQ(plan.asyncOverlapCandidateLevelCount, 0u);
    ASSERT_EQ(plan.queueSyncs.size(), 1u);
    EXPECT_EQ(plan.queueSyncCount, 1u);
    EXPECT_EQ(plan.crossQueueSyncCount, 1u);
    EXPECT_EQ(plan.queueBatches[1].prerequisiteSyncIndices, std::vector<uint32>({0u}));
    EXPECT_EQ(plan.queueSyncs[0].sourceBatchIndex, 0u);
    EXPECT_EQ(plan.queueSyncs[0].targetBatchIndex, 1u);
    EXPECT_EQ(plan.queueSyncs[0].sourcePassIndex, 0u);
    EXPECT_EQ(plan.queueSyncs[0].targetPassIndex, 1u);

    RenderGraph::Diagnostics diagnostics = graph.GetDiagnostics();
    EXPECT_EQ(diagnostics.plannedQueueBatchCount, plan.queueBatchCount);
    EXPECT_EQ(diagnostics.plannedDependencyLevelCount, plan.dependencyLevelCount);
    EXPECT_EQ(diagnostics.plannedComputeBatchCount, plan.computeBatchCount);
    EXPECT_EQ(diagnostics.plannedQueueSyncCount, plan.queueSyncCount);
    EXPECT_EQ(diagnostics.plannedCrossQueueSyncCount, plan.crossQueueSyncCount);
    ASSERT_EQ(diagnostics.plannedQueueBatches.size(), plan.queueBatches.size());
    ASSERT_EQ(diagnostics.plannedQueueSyncs.size(), plan.queueSyncs.size());
    EXPECT_EQ(diagnostics.plannedQueueBatches[0].passIndices, plan.queueBatches[0].passIndices);
    EXPECT_EQ(diagnostics.plannedQueueBatches[1].prerequisiteSyncIndices,
              plan.queueBatches[1].prerequisiteSyncIndices);
    EXPECT_EQ(diagnostics.plannedQueueSyncs[0].sourceBatchIndex, plan.queueSyncs[0].sourceBatchIndex);
    EXPECT_EQ(diagnostics.plannedQueueSyncs[0].targetBatchIndex, plan.queueSyncs[0].targetBatchIndex);
}

TEST(RenderGraphValidation, ExecuteAsyncUsesSubmissionPlanForCrossQueueSyncStats)
{
    FakeDevice device;
    device.MutableCapabilities().supportsAsyncCompute = true;
    device.MutableCapabilities().supportsExplicitQueueFenceSignal = true;
    device.MutableCapabilities().supportsQueueFenceWait = true;

    RenderGraph graph;
    graph.SetDevice(&device);

    RHIBufferDesc bufferDesc;
    bufferDesc.size = 1024;
    bufferDesc.usage = RHIBufferUsage::Structured | RHIBufferUsage::UnorderedAccess;
    FakeBuffer bufferA(bufferDesc);
    FakeBuffer bufferB(bufferDesc);

    RGBufferHandle resourceA = graph.ImportBuffer(&bufferA, RHIResourceState::Common);
    RGBufferHandle resourceB = graph.ImportBuffer(&bufferB, RHIResourceState::Common);

    struct BufferPassData
    {
        RGBufferHandle buffer;
    };

    graph.AddPass<BufferPassData>(
        "PlanStatsGraphicsProduceA",
        RenderGraphPassType::Graphics,
        [resourceA](RenderGraphBuilder& builder, BufferPassData& data)
        {
            data.buffer = builder.Write(resourceA, RHIResourceState::UnorderedAccess);
        },
        [](const BufferPassData&, RHICommandContext&) {});

    graph.AddPass<BufferPassData>(
        "PlanStatsGraphicsProduceB",
        RenderGraphPassType::Graphics,
        [resourceB](RenderGraphBuilder& builder, BufferPassData& data)
        {
            data.buffer = builder.Write(resourceB, RHIResourceState::UnorderedAccess);
        },
        [](const BufferPassData&, RHICommandContext&) {});

    graph.AddPass<BufferPassData>(
        "PlanStatsComputeConsumeA",
        RenderGraphPassType::Compute,
        [resourceA](RenderGraphBuilder& builder, BufferPassData& data)
        {
            data.buffer = builder.ReadWrite(resourceA);
        },
        [](const BufferPassData&, RHICommandContext&) {});

    graph.AddPass<BufferPassData>(
        "PlanStatsComputeConsumeB",
        RenderGraphPassType::Compute,
        [resourceB](RenderGraphBuilder& builder, BufferPassData& data)
        {
            data.buffer = builder.ReadWrite(resourceB);
        },
        [](const BufferPassData&, RHICommandContext&) {});

    graph.SetExportState(resourceA, RHIResourceState::ShaderResource);
    graph.SetExportState(resourceB, RHIResourceState::ShaderResource);
    graph.Compile();
    ASSERT_TRUE(graph.GetCompileStats().compileValid);

    RenderGraph::SubmissionPlan plan = graph.GetSubmissionPlan();
    ASSERT_EQ(plan.queueBatches.size(), 2u);
    EXPECT_EQ(plan.queueBatches[0].passIndices, std::vector<uint32>({0u, 1u}));
    EXPECT_EQ(plan.queueBatches[1].passIndices, std::vector<uint32>({2u, 3u}));
    EXPECT_EQ(plan.queueSyncCount, 1u);
    EXPECT_EQ(plan.crossQueueSyncCount, 1u);

    FakeCommandContext graphicsCtx;
    FakeCommandContext computeCtx;
    FakeFence computeFence;
    graph.ExecuteAsync(graphicsCtx, &computeCtx, &computeFence, 31);

    const RenderGraph::CompileStats& stats = graph.GetCompileStats();
    EXPECT_FALSE(stats.asyncFallbackUsed);
    EXPECT_EQ(stats.asyncComputeEligiblePasses, 2u);
    EXPECT_EQ(stats.asyncComputeScheduledPasses, 2u);
    EXPECT_EQ(stats.asyncGraphicsScheduledPasses, 2u);
    EXPECT_EQ(stats.asyncCrossQueueDependencyCount, plan.crossQueueSyncCount);
    EXPECT_EQ(stats.asyncFenceSignalCount, 2u);
    EXPECT_EQ(stats.asyncFenceWaitCount, 2u);
    EXPECT_EQ(stats.asyncFinalQueueJoinCount, 1u);

    RenderGraph::Diagnostics diagnostics = graph.GetDiagnostics();
    EXPECT_EQ(diagnostics.plannedQueueSyncCount, plan.queueSyncCount);
    EXPECT_EQ(diagnostics.plannedCrossQueueSyncCount, stats.asyncCrossQueueDependencyCount);
    EXPECT_EQ(diagnostics.plannedQueueSyncCoveredCount, 1u);
    EXPECT_EQ(diagnostics.plannedQueueSyncUncoveredCount, 0u);
    EXPECT_EQ(diagnostics.actualMatchedPlannedSyncCount, 1u);
    EXPECT_EQ(diagnostics.actualUnplannedQueueSyncCount, 1u);
    ASSERT_EQ(diagnostics.queueSyncs.size(), 2u);
    EXPECT_TRUE(diagnostics.queueSyncs[0].coversPlannedSync);
    EXPECT_EQ(diagnostics.queueSyncs[0].plannedSyncIndex, 0u);
    EXPECT_EQ(diagnostics.queueSyncs[1].reason, RenderGraph::DiagnosticSyncReason::FinalQueueJoin);
}

TEST(RenderGraphValidation, DiagnosticsReportsPlannedActualSyncCoverage)
{
    FakeDevice device;
    device.MutableCapabilities().supportsAsyncCompute = true;
    device.MutableCapabilities().supportsExplicitQueueFenceSignal = true;
    device.MutableCapabilities().supportsQueueFenceWait = true;

    RenderGraph graph;
    graph.SetDevice(&device);

    RHIBufferDesc bufferDesc;
    bufferDesc.size = 1024;
    bufferDesc.usage = RHIBufferUsage::Structured | RHIBufferUsage::UnorderedAccess;
    FakeBuffer buffer(bufferDesc);

    RGBufferHandle imported = graph.ImportBuffer(&buffer, RHIResourceState::Common);
    FakeCommandContext graphicsCtx;
    FakeCommandContext computeCtx;
    FakeFence computeFence;

    struct BufferPassData
    {
        RGBufferHandle buffer;
    };

    graph.AddPass<BufferPassData>(
        "CoverageGraphicsProduce",
        RenderGraphPassType::Graphics,
        [imported](RenderGraphBuilder& builder, BufferPassData& data)
        {
            data.buffer = builder.Write(imported, RHIResourceState::UnorderedAccess);
        },
        [](const BufferPassData&, RHICommandContext&) {});

    graph.AddPass<BufferPassData>(
        "CoverageComputeConsume",
        RenderGraphPassType::Compute,
        [imported](RenderGraphBuilder& builder, BufferPassData& data)
        {
            data.buffer = builder.ReadWrite(imported);
        },
        [](const BufferPassData&, RHICommandContext&) {});

    graph.SetExportState(imported, RHIResourceState::ShaderResource);
    graph.Compile();
    ASSERT_TRUE(graph.GetCompileStats().compileValid);

    graph.ExecuteAsync(graphicsCtx, &computeCtx, &computeFence, 23);

    RenderGraph::Diagnostics diagnostics = graph.GetDiagnostics();
    ASSERT_EQ(diagnostics.plannedQueueSyncs.size(), 1u);
    ASSERT_EQ(diagnostics.queueSyncs.size(), 2u);
    EXPECT_EQ(diagnostics.plannedQueueSyncCoveredCount, 1u);
    EXPECT_EQ(diagnostics.plannedQueueSyncUncoveredCount, 0u);
    EXPECT_TRUE(diagnostics.plannedQueueSyncs[0].coveredByActualSync);
    EXPECT_EQ(diagnostics.plannedQueueSyncs[0].actualSyncIndex, 0u);

    EXPECT_EQ(diagnostics.actualMatchedPlannedSyncCount, 1u);
    EXPECT_EQ(diagnostics.actualUnplannedQueueSyncCount, 1u);
    EXPECT_EQ(diagnostics.actualConservativeFinalJoinCount, 1u);
    EXPECT_TRUE(diagnostics.queueSyncs[0].coversPlannedSync);
    EXPECT_EQ(diagnostics.queueSyncs[0].plannedSyncIndex, 0u);
    EXPECT_FALSE(diagnostics.queueSyncs[1].coversPlannedSync);
    EXPECT_EQ(diagnostics.queueSyncs[1].plannedSyncIndex, RVX_INVALID_INDEX);
    EXPECT_EQ(diagnostics.queueSyncs[1].reason, RenderGraph::DiagnosticSyncReason::FinalQueueJoin);

    std::string dump = graph.ExportDiagnosticsText();
    EXPECT_NE(dump.find("plannedCoveredSyncs=1"), std::string::npos);
    EXPECT_NE(dump.find("plannedUncoveredSyncs=0"), std::string::npos);
    EXPECT_NE(dump.find("actualMatchedPlannedSyncs=1"), std::string::npos);
    EXPECT_NE(dump.find("actualUnplannedSyncs=1"), std::string::npos);
    EXPECT_NE(dump.find("actualConservativeFinalJoins=1"), std::string::npos);
    EXPECT_NE(dump.find("covered=true actualSync=0"), std::string::npos);
    EXPECT_NE(dump.find("coversPlanned=true plannedSync=0"), std::string::npos);
}

TEST(RenderGraphValidation, DiagnosticsReportsAsyncEfficiencyStats)
{
    FakeDevice device;
    device.MutableCapabilities().supportsAsyncCompute = true;
    device.MutableCapabilities().supportsExplicitQueueFenceSignal = true;
    device.MutableCapabilities().supportsQueueFenceWait = true;

    RenderGraph graph;
    graph.SetDevice(&device);

    RHIBufferDesc graphicsBufferDesc;
    graphicsBufferDesc.size = 1024;
    graphicsBufferDesc.usage = RHIBufferUsage::Structured | RHIBufferUsage::UnorderedAccess;
    FakeBuffer graphicsBuffer(graphicsBufferDesc);

    RHIBufferDesc computeBufferDesc;
    computeBufferDesc.size = 1024;
    computeBufferDesc.usage = RHIBufferUsage::Structured | RHIBufferUsage::UnorderedAccess;
    FakeBuffer computeBuffer(computeBufferDesc);

    RGBufferHandle graphicsResource = graph.ImportBuffer(&graphicsBuffer, RHIResourceState::Common);
    RGBufferHandle computeResource = graph.ImportBuffer(&computeBuffer, RHIResourceState::Common);

    struct BufferPassData
    {
        RGBufferHandle buffer;
    };

    graph.AddPass<BufferPassData>(
        "EfficiencyGraphicsProduce",
        RenderGraphPassType::Graphics,
        [graphicsResource](RenderGraphBuilder& builder, BufferPassData& data)
        {
            data.buffer = builder.Write(graphicsResource, RHIResourceState::UnorderedAccess);
        },
        [](const BufferPassData&, RHICommandContext&) {});

    graph.AddPass<BufferPassData>(
        "EfficiencyComputeIndependent",
        RenderGraphPassType::Compute,
        [computeResource](RenderGraphBuilder& builder, BufferPassData& data)
        {
            data.buffer = builder.Write(computeResource, RHIResourceState::UnorderedAccess);
        },
        [](const BufferPassData&, RHICommandContext&) {});

    graph.AddPass<BufferPassData>(
        "EfficiencyGraphicsConsume",
        RenderGraphPassType::Graphics,
        [graphicsResource](RenderGraphBuilder& builder, BufferPassData& data)
        {
            data.buffer = builder.ReadWrite(graphicsResource);
        },
        [](const BufferPassData&, RHICommandContext&) {});

    graph.SetExportState(graphicsResource, RHIResourceState::ShaderResource);
    graph.SetExportState(computeResource, RHIResourceState::ShaderResource);
    graph.Compile();
    ASSERT_TRUE(graph.GetCompileStats().compileValid);

    RenderGraph::Diagnostics plannedOnly = graph.GetDiagnostics();
    EXPECT_EQ(plannedOnly.plannedQueueBatchCount, 3u);
    EXPECT_EQ(plannedOnly.plannedDependencyLevelCount, 2u);
    EXPECT_EQ(plannedOnly.plannedComputeBatchCount, 1u);
    EXPECT_EQ(plannedOnly.plannedAsyncOverlapCandidateLevelCount, 1u);
    EXPECT_EQ(plannedOnly.actualQueueBatchCount, 0u);
    EXPECT_EQ(plannedOnly.actualQueueSwitchCount, 0u);
    EXPECT_EQ(plannedOnly.actualQueueSyncCount, 0u);
    EXPECT_EQ(plannedOnly.actualCrossQueueSyncCount, 0u);

    FakeCommandContext graphicsCtx;
    FakeCommandContext computeCtx;
    FakeFence computeFence;
    graph.ExecuteAsync(graphicsCtx, &computeCtx, &computeFence, 17);

    RenderGraph::Diagnostics executed = graph.GetDiagnostics();
    EXPECT_EQ(executed.plannedQueueBatchCount, 3u);
    EXPECT_EQ(executed.plannedAsyncOverlapCandidateLevelCount, 1u);
    EXPECT_EQ(executed.actualQueueBatchCount, 3u);
    EXPECT_EQ(executed.actualQueueSwitchCount, 2u);
    EXPECT_EQ(executed.actualQueueSyncCount, 1u);
    EXPECT_EQ(executed.actualCrossQueueSyncCount, 0u);

    std::string dump = graph.ExportDiagnosticsText();
    EXPECT_NE(dump.find("Schedule efficiency"), std::string::npos);
    EXPECT_NE(dump.find("plannedBatches=3"), std::string::npos);
    EXPECT_NE(dump.find("plannedLevels=2"), std::string::npos);
    EXPECT_NE(dump.find("plannedOverlapLevels=1"), std::string::npos);
    EXPECT_NE(dump.find("actualBatches=3"), std::string::npos);
    EXPECT_NE(dump.find("actualSwitches=2"), std::string::npos);
    EXPECT_NE(dump.find("actualSyncs=1"), std::string::npos);
    EXPECT_NE(dump.find("actualCrossQueueSyncs=0"), std::string::npos);
}

TEST(RenderGraphValidation, ExecuteAsyncDoesNotFenceIndependentComputeAndGraphicsPasses)
{
    FakeDevice device;
    device.MutableCapabilities().supportsAsyncCompute = true;
    device.MutableCapabilities().supportsExplicitQueueFenceSignal = true;
    device.MutableCapabilities().supportsQueueFenceWait = true;

    RenderGraph graph;
    graph.SetDevice(&device);

    RHIBufferDesc graphicsBufferDesc;
    graphicsBufferDesc.size = 1024;
    graphicsBufferDesc.usage = RHIBufferUsage::Structured | RHIBufferUsage::UnorderedAccess;
    FakeBuffer graphicsBuffer(graphicsBufferDesc);

    RHIBufferDesc computeBufferDesc;
    computeBufferDesc.size = 1024;
    computeBufferDesc.usage = RHIBufferUsage::Structured | RHIBufferUsage::UnorderedAccess;
    FakeBuffer computeBuffer(computeBufferDesc);

    RGBufferHandle graphicsResource = graph.ImportBuffer(&graphicsBuffer, RHIResourceState::Common);
    RGBufferHandle computeResource = graph.ImportBuffer(&computeBuffer, RHIResourceState::Common);

    FakeCommandContext graphicsCtx;
    FakeCommandContext computeCtx;
    FakeFence computeFence;

    struct BufferPassData
    {
        RGBufferHandle buffer;
    };

    graph.AddPass<BufferPassData>(
        "GraphicsProduce",
        RenderGraphPassType::Graphics,
        [graphicsResource](RenderGraphBuilder& builder, BufferPassData& data)
        {
            data.buffer = builder.Write(graphicsResource, RHIResourceState::UnorderedAccess);
        },
        [](const BufferPassData&, RHICommandContext&) {});

    graph.AddPass<BufferPassData>(
        "ComputeIndependent",
        RenderGraphPassType::Compute,
        [computeResource](RenderGraphBuilder& builder, BufferPassData& data)
        {
            data.buffer = builder.Write(computeResource, RHIResourceState::UnorderedAccess);
        },
        [](const BufferPassData&, RHICommandContext&) {});

    graph.AddPass<BufferPassData>(
        "GraphicsConsume",
        RenderGraphPassType::Graphics,
        [graphicsResource](RenderGraphBuilder& builder, BufferPassData& data)
        {
            data.buffer = builder.ReadWrite(graphicsResource);
        },
        [](const BufferPassData&, RHICommandContext&) {});

    graph.SetExportState(graphicsResource, RHIResourceState::ShaderResource);
    graph.SetExportState(computeResource, RHIResourceState::ShaderResource);
    graph.Compile();
    ASSERT_TRUE(graph.GetCompileStats().compileValid);

    graph.ExecuteAsync(graphicsCtx, &computeCtx, &computeFence, 11);

    const auto& stats = graph.GetCompileStats();
    EXPECT_FALSE(stats.asyncFallbackUsed);
    EXPECT_EQ(stats.asyncComputeEligiblePasses, 1u);
    EXPECT_EQ(stats.asyncComputeScheduledPasses, 1u);
    EXPECT_EQ(stats.asyncGraphicsScheduledPasses, 2u);
    EXPECT_EQ(stats.asyncCrossQueueDependencyCount, 0u);
    EXPECT_EQ(stats.asyncFenceSignalCount, 1u);
    EXPECT_EQ(stats.asyncFenceWaitCount, 1u);
    EXPECT_EQ(stats.asyncFinalQueueJoinCount, 1u);
    EXPECT_EQ(stats.lastExecutedPassCount, 3u);

    ASSERT_EQ(graphicsCtx.events.size(), 2u);
    EXPECT_EQ(graphicsCtx.events[0], "GraphicsProduce");
    EXPECT_EQ(graphicsCtx.events[1], "GraphicsConsume");
    ASSERT_EQ(computeCtx.events.size(), 1u);
    EXPECT_EQ(computeCtx.events[0], "ComputeIndependent");

    EXPECT_TRUE(graphicsCtx.signaledFenceValues.empty());
    ASSERT_EQ(computeCtx.signaledFenceValues.size(), 1u);
    ASSERT_EQ(graphicsCtx.waitedFenceValues.size(), 1u);
    EXPECT_EQ(computeCtx.signaledFenceValues[0], graphicsCtx.waitedFenceValues[0]);
    EXPECT_TRUE(computeCtx.waitedFenceValues.empty());

    RenderGraph::Diagnostics diagnostics = graph.GetDiagnostics();
    ASSERT_EQ(diagnostics.passes.size(), 3u);
    EXPECT_TRUE(diagnostics.passes[1].dependencies.empty());
    EXPECT_TRUE(diagnostics.passes[1].dependents.empty());
    ASSERT_EQ(diagnostics.passes[2].dependencies.size(), 1u);
    EXPECT_EQ(diagnostics.passes[2].dependencies[0], 0u);
    EXPECT_EQ(diagnostics.passes[0].executionQueue, RenderGraph::DiagnosticExecutionQueue::Graphics);
    EXPECT_EQ(diagnostics.passes[1].executionQueue, RenderGraph::DiagnosticExecutionQueue::Compute);
    EXPECT_EQ(diagnostics.passes[2].executionQueue, RenderGraph::DiagnosticExecutionQueue::Graphics);
    EXPECT_EQ(diagnostics.passes[0].executionSerial, 0u);
    EXPECT_EQ(diagnostics.passes[1].executionSerial, 1u);
    EXPECT_EQ(diagnostics.passes[2].executionSerial, 2u);
    ASSERT_EQ(diagnostics.queueBatches.size(), 3u);
    EXPECT_EQ(diagnostics.queueBatches[0].queue, RenderGraph::DiagnosticExecutionQueue::Graphics);
    EXPECT_EQ(diagnostics.queueBatches[0].passIndices, std::vector<uint32>({0u}));
    EXPECT_EQ(diagnostics.queueBatches[1].queue, RenderGraph::DiagnosticExecutionQueue::Compute);
    EXPECT_EQ(diagnostics.queueBatches[1].passIndices, std::vector<uint32>({1u}));
    EXPECT_EQ(diagnostics.queueBatches[2].queue, RenderGraph::DiagnosticExecutionQueue::Graphics);
    EXPECT_EQ(diagnostics.queueBatches[2].passIndices, std::vector<uint32>({2u}));
    ASSERT_EQ(diagnostics.queueSyncs.size(), 1u);
    EXPECT_EQ(diagnostics.queueSyncs[0].sourceQueue, RenderGraph::DiagnosticExecutionQueue::Compute);
    EXPECT_EQ(diagnostics.queueSyncs[0].targetQueue, RenderGraph::DiagnosticExecutionQueue::Graphics);
    EXPECT_EQ(diagnostics.queueSyncs[0].reason, RenderGraph::DiagnosticSyncReason::FinalQueueJoin);
    EXPECT_EQ(diagnostics.queueSyncs[0].sourcePassIndex, 1u);
    EXPECT_EQ(diagnostics.queueSyncs[0].targetPassIndex, RVX_INVALID_INDEX);

    std::string dump = graph.ExportDiagnosticsText();
    EXPECT_NE(dump.find("crossQueueDeps=0"), std::string::npos);
    EXPECT_NE(dump.find("finalJoins=1"), std::string::npos);
    EXPECT_NE(dump.find("ComputeIndependent type=Compute culled=false executed=true queue=Compute serial=1"), std::string::npos);
    EXPECT_NE(dump.find("dependencies=[] dependents=[]"), std::string::npos);
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
