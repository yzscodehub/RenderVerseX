#include "Core/Core.h"
#include "Render/Graph/RenderGraph.h"
#include "Render/Graph/TransientResourcePool.h"
#include "Resources/RenderSubmissionTracker.h"
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <mutex>
#include <stdexcept>
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

    class FakeTextureView final : public RHITextureView
    {
    public:
        FakeTextureView(RHITexture* texture, const RHITextureViewDesc& desc)
            : m_texture(texture)
            , m_format(desc.format == RHIFormat::Unknown
                           ? texture->GetFormat()
                           : desc.format)
            , m_range(desc.subresourceRange)
        {
        }

        RHITexture* GetTexture() const override { return m_texture; }
        RHIFormat GetFormat() const override { return m_format; }
        const RHISubresourceRange& GetSubresourceRange() const override
        {
            return m_range;
        }

    private:
        RHITexture* m_texture = nullptr;
        RHIFormat m_format = RHIFormat::Unknown;
        RHISubresourceRange m_range;
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

    class FakeHeap final : public RHIHeap
    {
    public:
        explicit FakeHeap(const RHIHeapDesc& desc)
            : m_desc(desc)
        {
        }

        uint64 GetSize() const override { return m_desc.size; }
        RHIHeapType GetType() const override { return m_desc.type; }
        RHIHeapFlags GetFlags() const override { return m_desc.flags; }

    private:
        RHIHeapDesc m_desc;
    };

    class FakeCommandContext final : public RHICommandContext
    {
    public:
        explicit FakeCommandContext(
            RHICommandQueueType queueType = RHICommandQueueType::Graphics)
            : m_queueType(queueType)
        {
        }

        RHICommandQueueType GetQueueType() const override { return m_queueType; }
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
        void AliasingBarriers(
            std::span<const RHIResourceAliasingBarrier> barriers) override
        {
            aliasingBarriers.insert(aliasingBarriers.end(),
                                    barriers.begin(),
                                    barriers.end());
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
        std::vector<RHIResourceAliasingBarrier> aliasingBarriers;
        std::vector<uint64> signaledFenceValues;
        std::vector<uint64> waitedFenceValues;

    private:
        RHICommandQueueType m_queueType = RHICommandQueueType::Graphics;
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
        FakeDevice()
        {
            m_capabilities.queueTopology.logicalQueueDomains[
                static_cast<uint8>(RHICommandQueueType::Compute)] =
                GPUQueueDomain::Compute;
        }

        RHIBufferRef CreateBuffer(const RHIBufferDesc& desc) override
        {
            ++createBufferCount;
            return RHIBufferRef(new FakeBuffer(desc));
        }

        RHITextureRef CreateTexture(const RHITextureDesc& desc) override
        {
            ++textureCreateAttemptCount;
            if (createTextureCount >= failTextureCreationAfter)
            {
                return {};
            }
            ++createTextureCount;
            return RHITextureRef(new FakeTexture(desc));
        }

        RHITextureViewRef CreateTextureView(
            RHITexture* texture,
            const RHITextureViewDesc& desc = {}) override
        {
            ++textureViewCreateAttemptCount;
            if (!texture || createTextureViewCount >= failTextureViewCreationAfter)
                return {};
            ++createTextureViewCount;
            return RHITextureViewRef(new FakeTextureView(texture, desc));
        }
        RHISamplerRef CreateSampler(const RHISamplerDesc&) override { return {}; }
        RHIShaderRef CreateShader(const RHIShaderDesc&) override { return {}; }
        RHIHeapRef CreateHeap(const RHIHeapDesc& desc) override
        {
            return m_capabilities.supportsExplicitAliasingBarriers
                ? RHIHeapRef(new FakeHeap(desc))
                : RHIHeapRef{};
        }
        RHITextureRef CreatePlacedTexture(
            RHIHeap*, uint64, const RHITextureDesc& desc) override
        {
            if (!m_capabilities.supportsExplicitAliasingBarriers)
            {
                return {};
            }
            ++createPlacedTextureCount;
            return RHITextureRef(new FakeTexture(desc));
        }
        RHIBufferRef CreatePlacedBuffer(
            RHIHeap*, uint64, const RHIBufferDesc& desc) override
        {
            if (!m_capabilities.supportsExplicitAliasingBarriers)
            {
                return {};
            }
            ++createPlacedBufferCount;
            return RHIBufferRef(new FakeBuffer(desc));
        }
        MemoryRequirements GetTextureMemoryRequirements(const RHITextureDesc&) override
        {
            ++textureMemoryRequirementQueryCount;
            return m_capabilities.supportsExplicitAliasingBarriers
                ? MemoryRequirements{65536, 65536}
                : MemoryRequirements{};
        }
        MemoryRequirements GetBufferMemoryRequirements(const RHIBufferDesc&) override
        {
            ++bufferMemoryRequirementQueryCount;
            return m_capabilities.supportsExplicitAliasingBarriers
                ? MemoryRequirements{65536, 256}
                : MemoryRequirements{};
        }
        RHIDescriptorSetLayoutRef CreateDescriptorSetLayout(const RHIDescriptorSetLayoutDesc&) override { return {}; }
        RHIPipelineLayoutRef CreatePipelineLayout(const RHIPipelineLayoutDesc&) override { return {}; }
        RHIPipelineRef CreateGraphicsPipeline(const RHIGraphicsPipelineDesc&) override { return {}; }
        RHIPipelineRef CreateComputePipeline(const RHIComputePipelineDesc&) override { return {}; }
        RHIDescriptorSetRef CreateDescriptorSet(const RHIDescriptorSetDesc&) override { return {}; }
        RHIQueryPoolRef CreateQueryPool(const RHIQueryPoolDesc&) override { return {}; }
        RHICommandContextRef CreateCommandContext(RHICommandQueueType type) override
        {
            return RHICommandContextRef(new FakeCommandContext(type));
        }
        uint64 SubmitCommandContext(RHICommandContext*, RHIFence*) override { return 0; }
        uint64 SubmitCommandContexts(std::span<RHICommandContext* const>, RHIFence*) override { return 0; }
        uint64 SubmitQueuePlan(const RHIQueueSubmissionPlan& plan,
                               RHIFence*) override
        {
            lastSubmittedQueuePlan = plan;
            return ++lastQueuePlanFenceValue;
        }
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
        const RHICapabilities& GetCapabilities() const override
        {
            ++capabilityQueryCount;
            return m_capabilities;
        }
        RHIBackendType GetBackendType() const override { return RHIBackendType::DX12; }

        RHICapabilities& MutableCapabilities() { return m_capabilities; }

        uint32 createTextureCount = 0;
        uint32 createBufferCount = 0;
        uint32 createPlacedTextureCount = 0;
        uint32 createPlacedBufferCount = 0;
        uint32 createTextureViewCount = 0;
        uint32 textureViewCreateAttemptCount = 0;
        uint32 textureCreateAttemptCount = 0;
        uint32 failTextureCreationAfter =
            std::numeric_limits<uint32>::max();
        uint32 failTextureViewCreationAfter =
            std::numeric_limits<uint32>::max();
        uint32 textureMemoryRequirementQueryCount = 0;
        uint32 bufferMemoryRequirementQueryCount = 0;
        mutable uint32 capabilityQueryCount = 0;
        RHIQueueSubmissionPlan lastSubmittedQueuePlan;
        uint64 lastQueuePlanFenceValue = 0;

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

    class ScopedJobSystem final
    {
    public:
        explicit ScopedJobSystem(size_t workerCount)
            : m_restoreInitialized(JobSystem::Get().IsInitialized()),
              m_restoreWorkerCount(m_restoreInitialized
                                       ? JobSystem::Get().GetWorkerCount()
                                       : 0U)
        {
            JobSystem::Get().Shutdown();
            JobSystem::Get().Initialize(workerCount);
        }

        ~ScopedJobSystem()
        {
            JobSystem::Get().Shutdown();
            if (m_restoreInitialized)
                JobSystem::Get().Initialize(m_restoreWorkerCount);
        }

    private:
        bool m_restoreInitialized = false;
        size_t m_restoreWorkerCount = 0;
    };
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

TEST(RenderGraphValidation, ExplicitTextureViewIsDeclaredThenRealizedOnce)
{
    FakeDevice device;
    TransientResourcePool pool;
    pool.Initialize(&device);
    pool.BeginFrame();

    RenderGraph graph;
    graph.SetDevice(&device);
    graph.SetTransientResourcePool(&pool);
    RHITextureDesc depthDesc = RHITextureDesc::DepthStencil(
        128, 128, RHIFormat::D32_FLOAT);
    depthDesc.arraySize = 4;
    const RGTextureHandle depth = graph.CreateTexture(depthDesc);

    struct PassData
    {
        RGTextureViewHandle cascadeView;
        RGTextureHandle depth;
    };
    bool executed = false;
    graph.AddPass<PassData>(
        "ExplicitCascadeView",
        RenderGraphPassType::Graphics,
        [depth](RenderGraphBuilder& builder, PassData& data)
        {
            RHITextureViewDesc desc;
            desc.format = RHIFormat::D32_FLOAT;
            desc.dimension = RHITextureDimension::Texture2D;
            desc.subresourceRange = {
                0, 1, 2, 1, RHITextureAspect::Depth};
            desc.type = RHITextureViewType::DepthStencil;
            desc.debugName = "ExplicitCascadeLayer2";
            data.cascadeView = builder.CreateTextureView(depth, desc);
            data.cascadeView = builder.Write(
                data.cascadeView,
                MakeRGAccessDesc(
                    RHIResourceState::DepthWrite,
                    RHIShaderStage::None,
                    RHIDiscardIntent::Discard));
            data.depth = depth;
        },
        [&executed](const PassData& data, RenderGraphPassContext& context)
        {
            RHITextureView* view =
                context.GetTextureView(data.cascadeView);
            ASSERT_NE(view, nullptr);
            EXPECT_EQ(view->GetTexture(), context.GetTexture(data.depth));
            EXPECT_EQ(view->GetSubresourceRange().baseArrayLayer, 2u);
            EXPECT_EQ(view->GetSubresourceRange().arrayLayerCount, 1u);
            executed = true;
        });
    graph.SetExportState(depth, RHIResourceState::ShaderResource);
    graph.Compile();
    ASSERT_TRUE(graph.GetCompileStats().compileValid);
    EXPECT_EQ(device.createTextureCount, 0u);
    EXPECT_EQ(device.createTextureViewCount, 0u);

    FakeCommandContext commandContext;
    graph.Execute(commandContext);
    EXPECT_TRUE(executed);
    EXPECT_EQ(device.createTextureCount, 1u);
    EXPECT_EQ(device.createTextureViewCount, 1u);
    EXPECT_EQ(pool.GetStats().recordingTextureLeases, 1u);

    RenderGraphExecution execution = graph.TakeExecution();
    ASSERT_TRUE(execution);
    EXPECT_TRUE(execution.AbortUnsubmitted());
    EXPECT_EQ(pool.GetStats().recordingTextureLeases, 0u);
    graph.Clear();
    pool.EndFrame();
    pool.Shutdown();
}

TEST(RenderGraphValidation, TextureViewRealizationFailureRollsBackTextureLease)
{
    FakeDevice device;
    device.failTextureViewCreationAfter = 0u;
    TransientResourcePool pool;
    pool.Initialize(&device);
    pool.BeginFrame();

    RenderGraph graph;
    graph.SetDevice(&device);
    graph.SetTransientResourcePool(&pool);
    const RGTextureHandle color = graph.CreateTexture(
        RHITextureDesc::RenderTarget(
            64, 64, RHIFormat::RGBA8_UNORM));
    struct PassData
    {
        RGTextureViewHandle target;
    };
    graph.AddPass<PassData>(
        "FailViewRealization",
        RenderGraphPassType::Graphics,
        [color](RenderGraphBuilder& builder, PassData& data)
        {
            RHITextureViewDesc desc;
            desc.format = RHIFormat::RGBA8_UNORM;
            desc.dimension = RHITextureDimension::Texture2D;
            desc.subresourceRange = RHISubresourceRange::All();
            desc.type = RHITextureViewType::RenderTarget;
            data.target = builder.CreateTextureView(color, desc);
            data.target = builder.Write(
                data.target,
                MakeRGAccessDesc(RHIResourceState::RenderTarget));
        },
        [](const PassData&, RenderGraphPassContext&) {});
    graph.SetExportState(color, RHIResourceState::ShaderResource);
    graph.Compile();
    ASSERT_TRUE(graph.GetCompileStats().compileValid);

    FakeCommandContext commandContext;
    graph.Execute(commandContext);
    EXPECT_EQ(device.textureViewCreateAttemptCount, 1u);
    EXPECT_EQ(device.createTextureViewCount, 0u);
    EXPECT_EQ(pool.GetStats().recordingTextureLeases, 0u);
    EXPECT_EQ(pool.GetStats().leaseAbortCount, 1u);
    EXPECT_EQ(graph.GetCompileStats().partialRealizationRollbackCount, 1u);
    EXPECT_FALSE(graph.TakeExecution());

    graph.Clear();
    pool.EndFrame();
    pool.Shutdown();
}

TEST(RenderGraphValidation, TransientTextureViewsFollowPhysicalPoolSlotReuse)
{
    FakeDevice device;
    TransientResourcePool pool;
    pool.Initialize(&device);

    RenderGraph graph;
    graph.SetDevice(&device);
    graph.SetTransientResourcePool(&pool);

    const auto recordFrame = [&]()
    {
        graph.Clear();
        const RGTextureHandle color = graph.CreateTexture(
            RHITextureDesc::RenderTarget(
                64, 64, RHIFormat::RGBA8_UNORM));
        struct PassData
        {
            RGTextureViewHandle target;
        };
        graph.AddPass<PassData>(
            "PooledExplicitView",
            RenderGraphPassType::Graphics,
            [color](RenderGraphBuilder& builder, PassData& data)
            {
                RHITextureViewDesc desc;
                desc.format = RHIFormat::RGBA8_UNORM;
                desc.dimension = RHITextureDimension::Texture2D;
                desc.subresourceRange = RHISubresourceRange::All();
                desc.type = RHITextureViewType::RenderTarget;
                data.target = builder.Write(
                    builder.CreateTextureView(color, desc),
                    MakeRGAccessDesc(RHIResourceState::RenderTarget));
            },
            [](const PassData& data, RenderGraphPassContext& context)
            {
                ASSERT_NE(context.GetTextureView(data.target), nullptr);
            });
        graph.SetExportState(color, RHIResourceState::ShaderResource);
        graph.Compile();
        ASSERT_TRUE(graph.GetCompileStats().compileValid);
        FakeCommandContext commandContext;
        graph.Execute(commandContext);
        RenderGraphExecution execution = graph.TakeExecution();
        ASSERT_TRUE(execution);
        ASSERT_TRUE(execution.AbortUnsubmitted());
    };

    pool.BeginFrame();
    recordFrame();
    pool.EndFrame();
    EXPECT_EQ(device.createTextureCount, 1u);
    EXPECT_EQ(device.createTextureViewCount, 1u);
    EXPECT_EQ(pool.GetStats().textureViewMisses, 1u);
    EXPECT_EQ(pool.GetStats().textureViewHits, 0u);
    EXPECT_EQ(pool.GetStats().textureViewCount, 1u);

    pool.BeginFrame();
    recordFrame();
    pool.EndFrame();
    EXPECT_EQ(device.createTextureCount, 1u);
    EXPECT_EQ(device.createTextureViewCount, 1u);
    EXPECT_EQ(pool.GetStats().textureViewMisses, 0u);
    EXPECT_EQ(pool.GetStats().textureViewHits, 1u);
    EXPECT_EQ(pool.GetStats().textureViewCount, 1u);

    graph.Clear();
    pool.Shutdown();
}

TEST(RenderGraphValidation, CompileIsIdempotentAndAllocationFree)
{
    FakeDevice device;
    device.MutableCapabilities().supportsQueueSubmissionPlan = true;

    RenderGraph graph;
    graph.SetDevice(&device);
    ASSERT_TRUE(graph.SetQueueExecutionMode(
        RenderGraph::QueueExecutionMode::MultiQueue));

    const RGTextureHandle texture = graph.CreateTexture(
        RHITextureDesc::RenderTarget(
            64, 64, RHIFormat::RGBA8_UNORM));
    struct PassData
    {
        RGTextureHandle colorTarget;
    };
    graph.AddPass<PassData>(
        "AllocationFreeCompile",
        RenderGraphPassType::Graphics,
        [texture](RenderGraphBuilder& builder, PassData& data)
        {
            data.colorTarget = builder.Write(
                texture,
                MakeRGAccessDesc(
                    RHIResourceState::RenderTarget,
                    RHIShaderStage::None,
                    RHIDiscardIntent::Discard));
        },
        [](const PassData&, RHICommandContext&) {});
    graph.SetExportState(texture, RHIResourceState::ShaderResource);
    const uint32 capabilityQueriesBeforeCompile =
        device.capabilityQueryCount;

    RenderGraphCompileOptions compileOptions;
    compileOptions.queuePolicy = RGQueuePolicy::PreferMultiQueue;
    compileOptions.capabilities = device.MutableCapabilities();
    compileOptions.hasCapabilitySnapshot = true;

    graph.Compile(compileOptions);
    ASSERT_TRUE(graph.GetCompileStats().compileValid);
    const RenderGraph::SubmissionPlan firstPlan = graph.GetSubmissionPlan();
    const uint64 firstPlanHash = graph.GetCompileStats().planHash;
    EXPECT_NE(firstPlanHash, 0u);
    EXPECT_EQ(device.createTextureCount, 0u);
    EXPECT_EQ(device.createPlacedTextureCount, 0u);
    EXPECT_EQ(device.textureMemoryRequirementQueryCount, 0u);
    EXPECT_EQ(device.bufferMemoryRequirementQueryCount, 0u);
    EXPECT_EQ(device.capabilityQueryCount,
              capabilityQueriesBeforeCompile);
    EXPECT_EQ(graph.GetQueueExecutionMode(),
              RenderGraph::QueueExecutionMode::GraphicsOnly);
    ASSERT_EQ(firstPlan.queueBatches.size(), 1u);
    EXPECT_EQ(firstPlan.computeBatchCount, 0u);
    EXPECT_EQ(firstPlan.copyBatchCount, 0u);

    graph.Compile(compileOptions);
    ASSERT_TRUE(graph.GetCompileStats().compileValid);
    const RenderGraph::SubmissionPlan secondPlan = graph.GetSubmissionPlan();
    EXPECT_EQ(graph.GetCompileStats().planHash, firstPlanHash);
    EXPECT_EQ(device.createTextureCount, 0u);
    EXPECT_EQ(device.createPlacedTextureCount, 0u);
    EXPECT_EQ(device.textureMemoryRequirementQueryCount, 0u);
    EXPECT_EQ(device.bufferMemoryRequirementQueryCount, 0u);
    EXPECT_EQ(device.capabilityQueryCount,
              capabilityQueriesBeforeCompile);
    EXPECT_EQ(firstPlan.queueBatchCount, secondPlan.queueBatchCount);
    EXPECT_EQ(firstPlan.queueSyncCount, secondPlan.queueSyncCount);

    FakeCommandContext context;
    graph.Execute(context);
    EXPECT_EQ(device.createTextureCount, 1u);
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
        const RHIAccessSnapshot shaderReadAccess = MakeRHIAccessSnapshot(
            RHIResourceState::ShaderResource,
            RHIShaderStage::Pixel,
            GPUQueueDomain::Graphics);
        graph.SetExportAccess(texture, shaderReadAccess);
        graph.Compile();
        FakeCommandContext ctx;
        graph.Execute(ctx);
        return ctx.textureBarriers;
    };

    pool.BeginFrame();
    graph.Clear();
    const auto firstFrameBarriers = buildFrame();
    pool.EndFrame();

    EXPECT_EQ(1u, device.createTextureCount);
    EXPECT_EQ(1u, pool.GetStats().textureMisses);
    EXPECT_EQ(0u, pool.GetStats().textureHits);

    pool.BeginFrame();
    graph.Clear();
    const auto secondFrameBarriers = buildFrame();
    pool.EndFrame();

    EXPECT_EQ(1u, device.createTextureCount);
    EXPECT_EQ(1u, pool.GetStats().textureHits);
    EXPECT_EQ(0u, pool.GetStats().textureMisses);
    ASSERT_FALSE(firstFrameBarriers.empty());
    ASSERT_FALSE(secondFrameBarriers.empty());
    EXPECT_EQ(firstFrameBarriers.front().accessBefore.layout,
              RHIResourceLayout::Undefined);
    EXPECT_EQ(secondFrameBarriers.front().accessBefore.layout,
              RHIResourceLayout::ShaderReadOnly);
    EXPECT_EQ(secondFrameBarriers.front().accessBefore.contentValidity,
              RHIContentValidity::Valid);

    graph.Clear();
    pool.Shutdown();
}

TEST(RenderGraphValidation, TransientBufferLeasePreservesRangeSnapshot)
{
    FakeDevice device;
    TransientResourcePool pool;
    pool.Initialize(&device);

    RHIBufferDesc desc;
    desc.size = 4096;
    desc.usage = RHIBufferUsage::Structured | RHIBufferUsage::UnorderedAccess;

    pool.BeginFrame();
    TransientBufferLease first = pool.AcquireBufferLease(desc);
    ASSERT_TRUE(first);
    EXPECT_FALSE(first.reused);

    RHIBufferAccessSnapshot finalAccess = MakeRHIBufferAccessSnapshot(
        RHIResourceState::ShaderResource,
        RHIShaderStage::Compute,
        GPUQueueDomain::Compute,
        RHIContentValidity::Valid);
    finalAccess.rangeOverrides.push_back({
        1024,
        1024,
        MakeRHIAccessSnapshot(RHIResourceState::UnorderedAccess,
                              RHIShaderStage::Compute,
                              GPUQueueDomain::Compute,
                              RHIContentValidity::Valid)});
    pool.ReleaseBuffer(first.buffer, finalAccess);
    pool.EndFrame();

    pool.BeginFrame();
    TransientBufferLease second = pool.AcquireBufferLease(desc);
    ASSERT_TRUE(second);
    EXPECT_TRUE(second.reused);
    EXPECT_EQ(second.buffer, first.buffer);
    EXPECT_EQ(second.accessSnapshot, finalAccess);
    pool.ReleaseBuffer(second.buffer, second.accessSnapshot);
    pool.EndFrame();
    pool.Shutdown();
}

TEST(RenderGraphValidation, TransientTexturePoolSeparatesOptimizedClearIdentity)
{
    FakeDevice device;
    TransientResourcePool pool;
    pool.Initialize(&device);

    RHITextureDesc firstDesc = RHITextureDesc::RenderTarget(
        64, 64, RHIFormat::RGBA16_FLOAT);
    firstDesc.SetOptimizedClearColor({0.1f, 0.1f, 0.15f, 1.0f});
    RHITextureDesc secondDesc = firstDesc;
    secondDesc.SetOptimizedClearColor({0.0f, 0.0f, 0.0f, 1.0f});

    pool.BeginFrame();
    TransientTextureLease first = pool.AcquireTextureLease(firstDesc);
    ASSERT_TRUE(first);
    EXPECT_FALSE(first.reused);
    pool.ReleaseTexture(first.texture, first.accessSnapshot);

    TransientTextureLease second = pool.AcquireTextureLease(secondDesc);
    ASSERT_TRUE(second);
    EXPECT_FALSE(second.reused);
    EXPECT_NE(second.texture, first.texture);
    EXPECT_EQ(device.createTextureCount, 2u);
    pool.ReleaseTexture(second.texture, second.accessSnapshot);
    pool.EndFrame();
    pool.Shutdown();
}

TEST(RenderGraphValidation, TransientLeaseIsMoveOnlyGenerationSafeAndAbortable)
{
    static_assert(!std::is_copy_constructible_v<TransientTextureLease>);
    static_assert(!std::is_copy_assignable_v<TransientTextureLease>);
    static_assert(!std::is_copy_constructible_v<TransientBufferLease>);
    static_assert(!std::is_copy_assignable_v<TransientBufferLease>);

    FakeDevice device;
    TransientResourcePool pool;
    pool.Initialize(&device);
    pool.BeginFrame();

    const RHITextureDesc desc = RHITextureDesc::RenderTarget(
        64, 64, RHIFormat::RGBA8_UNORM);
    TransientTextureLease first = pool.AcquireTextureLease(desc);
    ASSERT_TRUE(first);
    const uint64 firstSlot = first.GetSlotId();
    const uint32 firstGeneration = first.GetGeneration();
    RHITexture* firstTexture = first.texture;
    EXPECT_EQ(pool.GetStats().recordingTextureLeases, 1u);

    TransientTextureLease moved = std::move(first);
    EXPECT_FALSE(first);
    ASSERT_TRUE(moved);
    EXPECT_TRUE(moved.AbortUnsubmitted());
    EXPECT_FALSE(moved.AbortUnsubmitted());
    EXPECT_EQ(pool.GetStats().recordingTextureLeases, 0u);
    EXPECT_EQ(pool.GetStats().leaseAbortCount, 1u);

    TransientTextureLease reused = pool.AcquireTextureLease(desc);
    ASSERT_TRUE(reused);
    EXPECT_TRUE(reused.reused);
    EXPECT_EQ(reused.texture, firstTexture);
    EXPECT_EQ(reused.GetSlotId(), firstSlot);
    EXPECT_NE(reused.GetGeneration(), firstGeneration);
    EXPECT_TRUE(reused.AbortUnsubmitted());
    pool.EndFrame();
    pool.Shutdown();
}

TEST(RenderGraphValidation, TransientBufferLeaseIsGenerationSafeAndAbortable)
{
    FakeDevice device;
    TransientResourcePool pool;
    pool.Initialize(&device);
    pool.BeginFrame();

    RHIBufferDesc desc;
    desc.size = 4096;
    desc.usage = RHIBufferUsage::Structured |
                 RHIBufferUsage::UnorderedAccess;
    TransientBufferLease first = pool.AcquireBufferLease(desc);
    ASSERT_TRUE(first);
    const uint64 firstSlot = first.GetSlotId();
    const uint32 firstGeneration = first.GetGeneration();
    RHIBuffer* firstBuffer = first.buffer;
    EXPECT_TRUE(first.AbortUnsubmitted());
    EXPECT_FALSE(first.AbortUnsubmitted());

    TransientBufferLease reused = pool.AcquireBufferLease(desc);
    ASSERT_TRUE(reused);
    EXPECT_TRUE(reused.reused);
    EXPECT_EQ(reused.buffer, firstBuffer);
    EXPECT_EQ(reused.GetSlotId(), firstSlot);
    EXPECT_NE(reused.GetGeneration(), firstGeneration);
    EXPECT_TRUE(reused.AbortUnsubmitted());
    EXPECT_EQ(pool.GetStats().recordingBufferLeases, 0u);
    pool.EndFrame();
    pool.Shutdown();
}

TEST(RenderGraphValidation, DeviceLostLeaseNeverReturnsToFreePool)
{
    FakeDevice device;
    TransientResourcePool pool;
    pool.Initialize(&device);
    pool.BeginFrame();

    const RHITextureDesc desc = RHITextureDesc::RenderTarget(
        64, 64, RHIFormat::RGBA8_UNORM);
    TransientTextureLease lost = pool.AcquireTextureLease(desc);
    ASSERT_TRUE(lost);
    RHITexture* lostTexture = lost.texture;
    EXPECT_TRUE(lost.MarkDeviceLost());
    EXPECT_EQ(pool.GetStats().leaseDeviceLostCount, 1u);
    EXPECT_EQ(pool.GetStats().recordingTextureLeases, 0u);

    TransientTextureLease replacement = pool.AcquireTextureLease(desc);
    ASSERT_TRUE(replacement);
    EXPECT_FALSE(replacement.reused);
    EXPECT_NE(replacement.texture, lostTexture);
    EXPECT_EQ(device.createTextureCount, 2u);
    EXPECT_TRUE(replacement.AbortUnsubmitted());
    pool.EndFrame();
    pool.Shutdown();
}

TEST(RenderGraphValidation, TransientLeaseRequiresExactCompletionBeforeReuse)
{
    FakeDevice device;
    TransientResourcePool pool;
    pool.Initialize(&device);
    pool.BeginFrame();

    const RHITextureDesc desc = RHITextureDesc::RenderTarget(
        64, 64, RHIFormat::RGBA8_UNORM);
    TransientTextureLease first = pool.AcquireTextureLease(desc);
    ASSERT_TRUE(first);

    const GPUCompletionToken emptyCompletion;
    EXPECT_FALSE(first.CanCommit(emptyCompletion));
    EXPECT_FALSE(first.Commit(emptyCompletion, first.accessSnapshot));
    EXPECT_EQ(pool.GetStats().recordingTextureLeases, 1u);

    GPUCompletionToken completion;
    ASSERT_TRUE(InsertGPUCompletionPoint(
        completion, {GPUQueueDomain::Graphics, 7u}));
    ASSERT_TRUE(first.CanCommit(completion));
    const RHITextureAccessSnapshot finalAccess =
        MakeRHITextureAccessSnapshot(
            RHIResourceState::ShaderResource,
            RHIShaderStage::Pixel,
            GPUQueueDomain::Graphics,
            RHIContentValidity::Valid);
    EXPECT_TRUE(first.Commit(completion, finalAccess));
    EXPECT_EQ(pool.GetStats().recordingTextureLeases, 0u);
    EXPECT_EQ(pool.GetStats().inFlightTextureLeases, 1u);
    EXPECT_EQ(pool.GetStats().leaseCommitCount, 1u);

    // No tracker can prove token 7 complete, so the in-flight slot is not
    // reusable and a distinct physical texture must be allocated.
    TransientTextureLease second = pool.AcquireTextureLease(desc);
    ASSERT_TRUE(second);
    EXPECT_FALSE(second.reused);
    EXPECT_EQ(device.createTextureCount, 2u);
    EXPECT_TRUE(second.AbortUnsubmitted());
    pool.EndFrame();
    pool.Shutdown();
}

TEST(RenderGraphValidation, RenderGraphExecutionAbortReturnsEveryLease)
{
    FakeDevice device;
    TransientResourcePool pool;
    pool.Initialize(&device);
    pool.BeginFrame();

    RenderGraph graph;
    graph.SetDevice(&device);
    graph.SetTransientResourcePool(&pool);
    const RGTextureHandle texture = graph.CreateTexture(
        RHITextureDesc::RenderTarget(
            64, 64, RHIFormat::RGBA8_UNORM));
    struct PassData
    {
        RGTextureHandle output;
    };
    graph.AddPass<PassData>(
        "AbortExecution",
        RenderGraphPassType::Graphics,
        [texture](RenderGraphBuilder& builder, PassData& data)
        {
            data.output = builder.Write(texture);
        },
        [](const PassData&, RHICommandContext&) {});
    graph.SetExportState(texture, RHIResourceState::ShaderResource);
    graph.Compile();
    ASSERT_TRUE(graph.GetCompileStats().compileValid);
    FakeCommandContext context;
    graph.Execute(context);
    RHITexture* physicalTexture = graph.GetTexture(texture);
    ASSERT_NE(physicalTexture, nullptr);
    EXPECT_EQ(pool.GetStats().recordingTextureLeases, 1u);

    RenderGraphExecution execution = graph.TakeExecution();
    ASSERT_TRUE(execution);
    EXPECT_EQ(execution.GetState(), RenderGraphExecutionState::Recorded);
    EXPECT_TRUE(execution.AbortUnsubmitted());
    EXPECT_EQ(execution.GetState(),
              RenderGraphExecutionState::AbortUnsubmitted);
    EXPECT_EQ(pool.GetStats().recordingTextureLeases, 0u);

    TransientTextureLease reused = pool.AcquireTextureLease(
        RHITextureDesc::RenderTarget(
            64, 64, RHIFormat::RGBA8_UNORM));
    ASSERT_TRUE(reused);
    EXPECT_TRUE(reused.reused);
    EXPECT_EQ(reused.texture, physicalTexture);
    EXPECT_EQ(reused.accessSnapshot.uniformAccess.layout,
              RHIResourceLayout::Undefined);
    EXPECT_TRUE(reused.AbortUnsubmitted());

    graph.Clear();
    pool.EndFrame();
    pool.Shutdown();
}

TEST(RenderGraphValidation, PartialPhysicalRealizationRollsBackAllLeases)
{
    FakeDevice device;
    device.failTextureCreationAfter = 1u;
    TransientResourcePool pool;
    pool.Initialize(&device);
    pool.BeginFrame();

    RenderGraph graph;
    graph.SetDevice(&device);
    graph.SetTransientResourcePool(&pool);
    const RGTextureHandle first = graph.CreateTexture(
        RHITextureDesc::RenderTarget(
            64, 64, RHIFormat::RGBA8_UNORM));
    const RGTextureHandle second = graph.CreateTexture(
        RHITextureDesc::RenderTarget(
            128, 128, RHIFormat::RGBA8_UNORM));
    struct PassData
    {
        RGTextureHandle first;
        RGTextureHandle second;
    };
    graph.AddPass<PassData>(
        "FailSecondRealization",
        RenderGraphPassType::Graphics,
        [first, second](RenderGraphBuilder& builder, PassData& data)
        {
            data.first = builder.Write(first);
            data.second = builder.Write(second);
        },
        [](const PassData&, RHICommandContext&) {});
    graph.SetExportState(first, RHIResourceState::ShaderResource);
    graph.SetExportState(second, RHIResourceState::ShaderResource);
    graph.Compile();
    ASSERT_TRUE(graph.GetCompileStats().compileValid);
    FakeCommandContext context;
    graph.Execute(context);

    EXPECT_EQ(pool.GetStats().recordingTextureLeases, 0u);
    EXPECT_EQ(pool.GetStats().texturesInUse, 0u);
    EXPECT_EQ(graph.GetCompileStats().partialRealizationRollbackCount, 1u);
    EXPECT_FALSE(graph.TakeExecution());
    graph.Clear();
    pool.EndFrame();
    pool.Shutdown();
}

TEST(RenderGraphValidation, PassRecordingFailureRollsBackEveryRealizedLease)
{
    FakeDevice device;
    TransientResourcePool pool;
    pool.Initialize(&device);
    pool.BeginFrame();

    RenderGraph graph;
    graph.SetDevice(&device);
    graph.SetTransientResourcePool(&pool);
    const RGTextureHandle texture = graph.CreateTexture(
        RHITextureDesc::RenderTarget(
            64, 64, RHIFormat::RGBA8_UNORM));
    RHIBufferDesc bufferDesc;
    bufferDesc.size = 4096;
    bufferDesc.usage = RHIBufferUsage::Structured |
                       RHIBufferUsage::UnorderedAccess;
    const RGBufferHandle buffer = graph.CreateBuffer(bufferDesc);

    struct PassData
    {
        RGTextureHandle texture;
        RGBufferHandle buffer;
    };
    graph.AddPass<PassData>(
        "ThrowDuringRecording",
        RenderGraphPassType::Graphics,
        [texture, buffer](RenderGraphBuilder& builder, PassData& data)
        {
            data.texture = builder.Write(texture);
            data.buffer = builder.Write(buffer);
        },
        [](const PassData&, RHICommandContext&)
        {
            throw std::runtime_error("injected recording failure");
        });
    graph.SetExportState(texture, RHIResourceState::ShaderResource);
    graph.SetExportState(buffer, RHIResourceState::ShaderResource);
    graph.Compile();
    ASSERT_TRUE(graph.GetCompileStats().compileValid);

    FakeCommandContext context;
    graph.Execute(context);
    EXPECT_EQ(pool.GetStats().recordingTextureLeases, 0u);
    EXPECT_EQ(pool.GetStats().recordingBufferLeases, 0u);
    EXPECT_EQ(pool.GetStats().leaseAbortCount, 2u);
    EXPECT_EQ(graph.GetCompileStats().partialRealizationRollbackCount, 1u);
    EXPECT_FALSE(graph.TakeExecution());

    TransientTextureLease reusedTexture = pool.AcquireTextureLease(
        RHITextureDesc::RenderTarget(
            64, 64, RHIFormat::RGBA8_UNORM));
    ASSERT_TRUE(reusedTexture);
    EXPECT_TRUE(reusedTexture.reused);
    EXPECT_TRUE(reusedTexture.AbortUnsubmitted());
    TransientBufferLease reusedBuffer = pool.AcquireBufferLease(bufferDesc);
    ASSERT_TRUE(reusedBuffer);
    EXPECT_TRUE(reusedBuffer.reused);
    EXPECT_TRUE(reusedBuffer.AbortUnsubmitted());

    graph.Clear();
    pool.EndFrame();
    pool.Shutdown();
}

TEST(RenderGraphValidation, ScopedSameLayoutWriteDependenciesArePreserved)
{
    FakeDevice device;
    device.MutableCapabilities().queueTopology.logicalQueueDomains[
        static_cast<uint8>(RHICommandQueueType::Compute)] = GPUQueueDomain::Compute;

    RenderGraph graph;
    graph.SetDevice(&device);

    RHIBufferDesc desc;
    desc.size = 1024;
    desc.usage = RHIBufferUsage::Structured | RHIBufferUsage::UnorderedAccess;
    FakeBuffer buffer(desc);

    const RHIAccessSnapshot unorderedAccess = MakeRHIAccessSnapshot(
        RHIResourceState::UnorderedAccess,
        RHIShaderStage::Compute,
        GPUQueueDomain::Graphics,
        RHIContentValidity::Valid);
    RGBufferHandle handle = graph.ImportBuffer(
        &buffer,
        RHIBufferAccessSnapshot{unorderedAccess, {}});

    struct PassData { RGBufferHandle buffer; };
    graph.AddPass<PassData>(
        "WriteA",
        RenderGraphPassType::Compute,
        [handle, unorderedAccess](RenderGraphBuilder& builder, PassData& data)
        {
            data.buffer = builder.Write(handle, unorderedAccess);
        },
        [](const PassData&, RHICommandContext&) {});
    graph.AddPass<PassData>(
        "WriteB",
        RenderGraphPassType::Compute,
        [handle, unorderedAccess](RenderGraphBuilder& builder, PassData& data)
        {
            data.buffer = builder.Write(handle, unorderedAccess);
        },
        [](const PassData&, RHICommandContext&) {});
    RHIAccessSnapshot generalShaderRead = unorderedAccess;
    generalShaderRead.memoryAccess = RHIMemoryAccess::ShaderRead;
    graph.AddPass<PassData>(
        "ReadGeneral",
        RenderGraphPassType::Compute,
        [handle, generalShaderRead](RenderGraphBuilder& builder, PassData& data)
        {
            data.buffer = builder.Read(handle, generalShaderRead);
        },
        [](const PassData&, RHICommandContext&) {});
    graph.SetExportAccess(handle, generalShaderRead);
    graph.Compile();
    ASSERT_TRUE(graph.GetCompileStats().compileValid);

    FakeCommandContext ctx;
    graph.Execute(ctx);
    ASSERT_GE(ctx.bufferBarriers.size(), 2u);
    for (const RHIBufferBarrier& barrier : ctx.bufferBarriers)
    {
        EXPECT_TRUE(barrier.hasScopedAccess);
        EXPECT_EQ(barrier.stateBefore, barrier.stateAfter);
        EXPECT_TRUE(HasDependencyKind(
            barrier.dependencyKind,
            RHIDependencyKind::Memory));
    }
}

TEST(RenderGraphValidation, DiscardIntentKeepsActualBeforeSnapshot)
{
    FakeDevice device;
    RenderGraph graph;
    graph.SetDevice(&device);

    RHITextureDesc desc = RHITextureDesc::RenderTarget(
        32, 32, RHIFormat::RGBA8_UNORM);
    RGTextureHandle texture = graph.CreateTexture(desc);
    const RHIAccessSnapshot renderTargetAccess = MakeRHIAccessSnapshot(
        RHIResourceState::RenderTarget,
        RHIShaderStage::None,
        GPUQueueDomain::Graphics,
        RHIContentValidity::Valid);

    struct PassData { RGTextureHandle texture; };
    graph.AddPass<PassData>(
        "DiscardWrite",
        RenderGraphPassType::Graphics,
        [texture, renderTargetAccess](RenderGraphBuilder& builder, PassData& data)
        {
            data.texture = builder.Write(
                texture,
                renderTargetAccess,
                RHIDiscardIntent::Discard);
        },
        [](const PassData&, RHICommandContext&) {});
    graph.SetExportAccess(texture, renderTargetAccess);
    graph.Compile();
    ASSERT_TRUE(graph.GetCompileStats().compileValid);

    FakeCommandContext ctx;
    graph.Execute(ctx);
    ASSERT_FALSE(ctx.textureBarriers.empty());
    const RHITextureBarrier& barrier = ctx.textureBarriers.front();
    EXPECT_EQ(barrier.accessBefore.layout, RHIResourceLayout::Undefined);
    EXPECT_EQ(barrier.accessBefore.contentValidity, RHIContentValidity::Invalid);
    EXPECT_EQ(barrier.discardIntent, RHIDiscardIntent::Discard);
    EXPECT_TRUE(HasDependencyKind(
        barrier.dependencyKind,
        RHIDependencyKind::Discard));
}

TEST(RenderGraphValidation, FullExportAggregatesSubresourceContentValidity)
{
    FakeDevice device;
    RenderGraph graph;
    graph.SetDevice(&device);

    RHITextureDesc desc = RHITextureDesc::RenderTarget(
        32, 32, RHIFormat::RGBA8_UNORM);
    desc.mipLevels = 2;
    RGTextureHandle texture = graph.CreateTexture(desc);
    const RHIAccessSnapshot renderTargetAccess = MakeRHIAccessSnapshot(
        RHIResourceState::RenderTarget,
        RHIShaderStage::None,
        GPUQueueDomain::Graphics,
        RHIContentValidity::Valid);

    struct PassData { RGTextureHandle texture; };
    for (uint32 mip = 0; mip < desc.mipLevels; ++mip)
    {
        RGTextureHandle mipHandle = texture.Subresource(mip, 0);
        graph.AddPass<PassData>(
            mip == 0 ? "WriteMip0" : "WriteMip1",
            RenderGraphPassType::Graphics,
            [mipHandle, renderTargetAccess](RenderGraphBuilder& builder, PassData& data)
            {
                data.texture = builder.Write(
                    mipHandle,
                    renderTargetAccess,
                    RHIDiscardIntent::Discard);
            },
            [](const PassData&, RHICommandContext&) {});
    }

    graph.SetExportAccess(
        texture,
        MakeRHIAccessSnapshot(RHIResourceState::ShaderResource,
                              RHIShaderStage::Pixel,
                              GPUQueueDomain::Graphics));
    graph.Compile();
    ASSERT_TRUE(graph.GetCompileStats().compileValid);

    FakeCommandContext ctx;
    graph.Execute(ctx);
    EXPECT_EQ(graph.GetRealizedAccess(texture).uniformAccess.contentValidity,
              RHIContentValidity::Valid);
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
    EXPECT_NE(dump.find("Schema: 6"), std::string::npos);
    EXPECT_NE(dump.find(RVX_RENDER_GRAPH_DIAGNOSTICS_SCHEMA_ID), std::string::npos);
    EXPECT_NE(dump.find("ProduceColor"), std::string::npos);
    EXPECT_NE(dump.find("DiagnosticColor"), std::string::npos);
    EXPECT_NE(dump.find("Estimated transient memory"), std::string::npos);

    std::string json = graph.ExportDiagnosticsJson();
    EXPECT_NE(json.find("\"schemaVersion\": 6"), std::string::npos);
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

TEST(RenderGraphValidation, ExplicitAliasingReusesMemoryAndEmitsOwnershipBarrier)
{
    FakeDevice device;
    device.MutableCapabilities().supportsExplicitResourceBarriers = true;
    device.MutableCapabilities().supportsExplicitAliasingBarriers = true;

    RenderGraph graph;
    graph.SetDevice(&device);
    graph.SetMemoryAliasingEnabled(true);

    const RHITextureDesc textureDesc =
        RHITextureDesc::RenderTarget(128, 128, RHIFormat::RGBA16_FLOAT);
    const RGTextureHandle first = graph.CreateTexture(textureDesc);
    const RGTextureHandle bridge = graph.CreateTexture(textureDesc);
    const RGTextureHandle reused = graph.CreateTexture(textureDesc);

    struct TwoTexturePass
    {
        RGTextureHandle input;
        RGTextureHandle output;
    };

    graph.AddPass<SimplePassData>(
        "ProduceFirst",
        RenderGraphPassType::Graphics,
        [&](RenderGraphBuilder& builder, SimplePassData& data)
        {
            data.colorTarget = builder.Write(
                first,
                RHIResourceState::RenderTarget);
        },
        [](const SimplePassData&, RHICommandContext&) {});
    graph.AddPass<TwoTexturePass>(
        "BridgeLifetime",
        RenderGraphPassType::Graphics,
        [&](RenderGraphBuilder& builder, TwoTexturePass& data)
        {
            data.input = builder.Read(first);
            data.output = builder.Write(
                bridge,
                RHIResourceState::RenderTarget);
        },
        [](const TwoTexturePass&, RHICommandContext&) {});
    graph.AddPass<TwoTexturePass>(
        "ReuseFirstAllocation",
        RenderGraphPassType::Graphics,
        [&](RenderGraphBuilder& builder, TwoTexturePass& data)
        {
            data.input = builder.Read(bridge);
            data.output = builder.Write(
                reused,
                RHIResourceState::RenderTarget);
        },
        [](const TwoTexturePass&, RHICommandContext&) {});

    graph.SetExportState(reused, RHIResourceState::ShaderResource);
    graph.Compile();

    const RenderGraph::CompileStats& stats = graph.GetCompileStats();
    EXPECT_TRUE(graph.IsMemoryAliasingEnabled());
    EXPECT_TRUE(stats.memoryAliasingEnabled);
    EXPECT_FALSE(stats.memoryAliasingUnsupportedRequested);
    EXPECT_TRUE(stats.explicitAliasingBarriersSupported);
    EXPECT_EQ(stats.aliasedTextureCount, 2u);
    EXPECT_EQ(stats.aliasedBufferCount, 0u);
    // Compilation is allocation-free even when aliasing is enabled. Physical
    // heaps and placed resources are realized only by execution.
    EXPECT_EQ(device.createPlacedTextureCount, 0u);
    EXPECT_LT(stats.memoryWithAliasing, stats.memoryWithoutAliasing);

    FakeCommandContext context;
    graph.Execute(context);

    EXPECT_EQ(device.createPlacedTextureCount, 3u);

    ASSERT_EQ(context.aliasingBarriers.size(), 1u);
    EXPECT_NE(context.aliasingBarriers[0].resourceBefore, nullptr);
    EXPECT_NE(context.aliasingBarriers[0].resourceAfter, nullptr);
    EXPECT_NE(context.aliasingBarriers[0].resourceBefore,
              context.aliasingBarriers[0].resourceAfter);
}

TEST(RenderGraphValidation, AliasingNeverOverlapsAStillLiveReplacement)
{
    FakeDevice device;
    device.MutableCapabilities().supportsExplicitResourceBarriers = true;
    device.MutableCapabilities().supportsExplicitAliasingBarriers = true;

    RenderGraph graph;
    graph.SetDevice(&device);
    graph.SetMemoryAliasingEnabled(true);

    const RHITextureDesc textureDesc =
        RHITextureDesc::RenderTarget(128, 128, RHIFormat::RGBA16_FLOAT);
    const RGTextureHandle first = graph.CreateTexture(textureDesc);
    const RGTextureHandle liveReplacement = graph.CreateTexture(textureDesc);
    const RGTextureHandle concurrent = graph.CreateTexture(textureDesc);

    graph.AddPass<SimplePassData>(
        "ProduceFirst",
        RenderGraphPassType::Graphics,
        [&](RenderGraphBuilder& builder, SimplePassData& data)
        {
            data.colorTarget = builder.Write(first);
        },
        [](const SimplePassData&, RHICommandContext&) {});
    graph.AddPass<SimplePassData>(
        "ReuseFirstAllocation",
        RenderGraphPassType::Graphics,
        [&](RenderGraphBuilder& builder, SimplePassData& data)
        {
            data.colorTarget = builder.Write(liveReplacement);
        },
        [](const SimplePassData&, RHICommandContext&) {});

    struct ReadWritePass
    {
        RGTextureHandle input;
        RGTextureHandle output;
    };
    graph.AddPass<ReadWritePass>(
        "KeepReplacementLive",
        RenderGraphPassType::Graphics,
        [&](RenderGraphBuilder& builder, ReadWritePass& data)
        {
            data.input = builder.Read(liveReplacement);
            data.output = builder.Write(concurrent);
        },
        [](const ReadWritePass&, RHICommandContext&) {});

    graph.SetExportState(first, RHIResourceState::ShaderResource);
    graph.SetExportState(concurrent, RHIResourceState::ShaderResource);
    graph.Compile();

    const RenderGraph::CompileStats& stats = graph.GetCompileStats();
    ASSERT_GT(stats.memoryWithoutAliasing, 0u);
    EXPECT_EQ(stats.memoryWithoutAliasing % 3u, 0u);
    EXPECT_EQ(stats.memoryWithAliasing * 3u,
              stats.memoryWithoutAliasing * 2u);
    EXPECT_EQ(stats.aliasedTextureCount, 2u);

    FakeCommandContext context;
    graph.Execute(context);
    EXPECT_EQ(context.aliasingBarriers.size(), 1u);
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

TEST(RenderGraphValidation, GraphicsOnlyModeKeepsComputePassAccessOnGraphicsDomain)
{
    FakeDevice device;
    device.MutableCapabilities().supportsAsyncCompute = true;
    device.MutableCapabilities().supportsExplicitQueueFenceSignal = true;
    device.MutableCapabilities().supportsQueueFenceWait = true;

    RenderGraph graph;
    graph.SetDevice(&device);
    EXPECT_EQ(graph.GetQueueExecutionMode(),
              RenderGraph::QueueExecutionMode::GraphicsOnly);

    RHIBufferDesc bufferDesc;
    bufferDesc.size = 1024;
    bufferDesc.usage = RHIBufferUsage::Structured |
        RHIBufferUsage::UnorderedAccess;
    FakeBuffer buffer(bufferDesc);
    const RGBufferHandle imported = graph.ImportBuffer(
        &buffer, RHIResourceState::Common);

    struct ComputeData
    {
        RGBufferHandle buffer;
    };
    graph.AddPass<ComputeData>(
        "GraphicsOnlyCompute",
        RenderGraphPassType::Compute,
        [imported](RenderGraphBuilder& builder, ComputeData& data)
        {
            data.buffer = builder.Write(
                imported, RHIResourceState::UnorderedAccess);
        },
        [](const ComputeData&, RHICommandContext&) {});
    graph.SetExportState(imported, RHIResourceState::ShaderResource);
    graph.Compile();

    const RenderGraph::Diagnostics planned = graph.GetDiagnostics();
    ASSERT_EQ(planned.passes.size(), 1u);
    EXPECT_EQ(planned.passes[0].plannedExecutionQueue,
              RenderGraph::DiagnosticExecutionQueue::Graphics);
    ASSERT_EQ(planned.passes[0].usages.size(), 1u);
    EXPECT_EQ(planned.passes[0].usages[0].desiredAccess.domain,
              GPUQueueDomain::Graphics);
    EXPECT_EQ(planned.plannedComputeBatchCount, 0u);

    FakeCommandContext graphicsCtx;
    graph.Execute(graphicsCtx);
    EXPECT_EQ(graph.GetCompileStats().executionQueueMismatchCount, 0u);
    EXPECT_EQ(graph.GetCompileStats().lastExecutedPassCount, 1u);
}

TEST(RenderGraphValidation, GraphicsExecuteRejectsExplicitAsyncComputePlan)
{
    FakeDevice device;
    device.MutableCapabilities().supportsAsyncCompute = true;
    device.MutableCapabilities().supportsExplicitQueueFenceSignal = true;
    device.MutableCapabilities().supportsQueueFenceWait = true;

    RenderGraph graph;
    graph.SetDevice(&device);
    ASSERT_TRUE(graph.SetQueueExecutionMode(
        RenderGraph::QueueExecutionMode::MultiQueue));

    RHIBufferDesc bufferDesc;
    bufferDesc.size = 1024;
    bufferDesc.usage = RHIBufferUsage::Structured |
        RHIBufferUsage::UnorderedAccess;
    FakeBuffer buffer(bufferDesc);
    const RGBufferHandle imported = graph.ImportBuffer(
        &buffer, RHIResourceState::Common);

    bool executed = false;
    struct ComputeData
    {
        RGBufferHandle buffer;
    };
    graph.AddPass<ComputeData>(
        "AsyncComputeMustNotRunOnGraphics",
        RenderGraphPassType::Compute,
        [imported](RenderGraphBuilder& builder, ComputeData& data)
        {
            data.buffer = builder.Write(
                imported, RHIResourceState::UnorderedAccess);
        },
        [&executed](const ComputeData&, RHICommandContext&)
        {
            executed = true;
        });
    graph.SetExportState(imported, RHIResourceState::ShaderResource);
    graph.Compile();

    FakeCommandContext graphicsCtx;
    graph.Execute(graphicsCtx);
    EXPECT_FALSE(executed);
    EXPECT_EQ(graph.GetCompileStats().executionQueueMismatchCount, 1u);
    EXPECT_EQ(graph.GetCompileStats().lastExecutedPassCount, 0u);
}

TEST(RenderGraphValidation, ExecuteAsyncFallsBackToGraphicsWhenBackendDoesNotSupportQueueSync)
{
    RenderGraph graph;
    ASSERT_TRUE(graph.SetQueueExecutionMode(
        RenderGraph::QueueExecutionMode::MultiQueue));

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
    EXPECT_EQ(stats.asyncFallbackReason,
              RenderGraph::AsyncComputeFallbackReason::AsyncPlanningDisabled);
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
    ASSERT_TRUE(graph.SetQueueExecutionMode(
        RenderGraph::QueueExecutionMode::MultiQueue));

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
    ASSERT_TRUE(graph.SetQueueExecutionMode(
        RenderGraph::QueueExecutionMode::MultiQueue));

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
    EXPECT_EQ(diagnostics.actualMatchedPlannedSyncCount, 2u);
    EXPECT_EQ(diagnostics.actualUnplannedQueueSyncCount, 0u);
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
    EXPECT_TRUE(diagnostics.queueSyncs[1].coversPlannedSync);
    EXPECT_EQ(diagnostics.queueSyncs[1].plannedSyncIndex, 1u);
    EXPECT_LT(diagnostics.queueSyncs[0].fenceValue, diagnostics.queueSyncs[1].fenceValue);

    std::string dump = graph.ExportDiagnosticsText();
    EXPECT_NE(dump.find("Queue Batches"), std::string::npos);
    EXPECT_NE(dump.find("Queue Syncs"), std::string::npos);
    EXPECT_NE(dump.find("[0] queue=Graphics serial=[0,0] passes=[0]"), std::string::npos);
    EXPECT_NE(dump.find("[1] queue=Compute serial=[1,1] passes=[1]"), std::string::npos);
    EXPECT_NE(dump.find("Graphics -> Compute reason=CrossQueueDependency"), std::string::npos);
    EXPECT_NE(dump.find("Compute -> Graphics reason=FinalQueueJoin"), std::string::npos);
    EXPECT_NE(dump.find("actualMatchedPlannedSyncs=2"), std::string::npos);
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
    ASSERT_TRUE(graph.SetQueueExecutionMode(
        RenderGraph::QueueExecutionMode::MultiQueue));

    RHIBufferDesc graphicsBufferDesc;
    graphicsBufferDesc.size = 1024;
    graphicsBufferDesc.usage = RHIBufferUsage::Structured | RHIBufferUsage::UnorderedAccess;
    FakeBuffer graphicsBuffer(graphicsBufferDesc);

    RHIBufferDesc computeBufferDesc;
    computeBufferDesc.size = 1024;
    computeBufferDesc.usage = RHIBufferUsage::Structured | RHIBufferUsage::UnorderedAccess;
    FakeBuffer computeBuffer(computeBufferDesc);

    RGBufferHandle graphicsResource = graph.ImportBuffer(&graphicsBuffer, RHIResourceState::Common);
    RGBufferHandle computeResource = graph.ImportBuffer(
        &computeBuffer,
        MakeRHIBufferAccessSnapshot(
            RHIResourceState::Common,
            RHIShaderStage::None,
            GPUQueueDomain::Compute,
            RHIContentValidity::Valid));

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
    ASSERT_EQ(diagnostics.plannedQueueBatches.size(), 4u);
    EXPECT_EQ(diagnostics.plannedQueueBatchCount, 4u);
    EXPECT_EQ(diagnostics.plannedDependencyLevelCount, 3u);
    EXPECT_EQ(diagnostics.plannedComputeBatchCount, 1u);
    EXPECT_EQ(diagnostics.plannedAsyncOverlapCandidateLevelCount, 1u);
    EXPECT_EQ(diagnostics.plannedQueueSyncCount, 1u);
    EXPECT_EQ(diagnostics.plannedCrossQueueSyncCount, 0u);
    ASSERT_EQ(diagnostics.plannedQueueSyncs.size(), 1u);
    EXPECT_EQ(diagnostics.plannedQueueSyncs[0].reason,
              RenderGraph::DiagnosticSyncReason::FinalQueueJoin);
    EXPECT_EQ(diagnostics.plannedQueueBatches[0].dependencyLevel, 0u);
    EXPECT_EQ(diagnostics.plannedQueueBatches[0].queue, RenderGraph::DiagnosticExecutionQueue::Graphics);
    EXPECT_EQ(diagnostics.plannedQueueBatches[0].passIndices, std::vector<uint32>({0u}));
    EXPECT_EQ(diagnostics.plannedQueueBatches[1].dependencyLevel, 0u);
    EXPECT_EQ(diagnostics.plannedQueueBatches[1].queue, RenderGraph::DiagnosticExecutionQueue::Compute);
    EXPECT_EQ(diagnostics.plannedQueueBatches[1].passIndices, std::vector<uint32>({1u}));
    EXPECT_EQ(diagnostics.plannedQueueBatches[2].dependencyLevel, 1u);
    EXPECT_EQ(diagnostics.plannedQueueBatches[2].queue, RenderGraph::DiagnosticExecutionQueue::Graphics);
    EXPECT_EQ(diagnostics.plannedQueueBatches[2].passIndices, std::vector<uint32>({2u}));
    EXPECT_TRUE(diagnostics.plannedQueueBatches[3].syntheticTerminal);
    EXPECT_EQ(diagnostics.plannedQueueBatches[3].queue,
              RenderGraph::DiagnosticExecutionQueue::Graphics);
    EXPECT_TRUE(diagnostics.plannedQueueBatches[3].passIndices.empty());

    EXPECT_TRUE(diagnostics.queueBatches.empty());
    EXPECT_TRUE(diagnostics.queueSyncs.empty());

    std::string dump = graph.ExportDiagnosticsText();
    EXPECT_NE(dump.find("Planned Queue Batches"), std::string::npos);
    EXPECT_NE(dump.find("plannedBatches=4"), std::string::npos);
    EXPECT_NE(dump.find("plannedOverlapLevels=1"), std::string::npos);
    EXPECT_NE(dump.find("plannedSyncs=1"), std::string::npos);
    EXPECT_NE(dump.find("[0] level=0 queue=Graphics passes=[0]"), std::string::npos);
    EXPECT_NE(dump.find("[1] level=0 queue=Compute passes=[1]"), std::string::npos);
    EXPECT_NE(dump.find("[2] level=1 queue=Graphics passes=[2]"), std::string::npos);
}

TEST(RenderGraphValidation, DiagnosticsReportsPlannedSubmissionSyncGraph)
{
    FakeDevice device;
    device.MutableCapabilities().supportsAsyncCompute = true;
    device.MutableCapabilities().supportsExplicitQueueFenceSignal = true;
    device.MutableCapabilities().supportsQueueFenceWait = true;
    RenderGraph graph;
    graph.SetDevice(&device);
    ASSERT_TRUE(graph.SetQueueExecutionMode(
        RenderGraph::QueueExecutionMode::MultiQueue));

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
    ASSERT_EQ(diagnostics.plannedQueueBatches.size(), 3u);
    EXPECT_EQ(diagnostics.plannedQueueBatches[0].queue, RenderGraph::DiagnosticExecutionQueue::Graphics);
    EXPECT_EQ(diagnostics.plannedQueueBatches[0].passIndices, std::vector<uint32>({0u}));
    EXPECT_TRUE(diagnostics.plannedQueueBatches[0].prerequisiteSyncIndices.empty());
    EXPECT_EQ(diagnostics.plannedQueueBatches[1].queue, RenderGraph::DiagnosticExecutionQueue::Compute);
    EXPECT_EQ(diagnostics.plannedQueueBatches[1].passIndices, std::vector<uint32>({1u}));
    EXPECT_EQ(diagnostics.plannedQueueBatches[1].prerequisiteSyncIndices, std::vector<uint32>({0u}));
    EXPECT_TRUE(diagnostics.plannedQueueBatches[2].syntheticTerminal);
    EXPECT_EQ(diagnostics.plannedQueueBatches[2].prerequisiteSyncIndices,
              std::vector<uint32>({1u}));

    ASSERT_EQ(diagnostics.plannedQueueSyncs.size(), 2u);
    EXPECT_EQ(diagnostics.plannedQueueSyncCount, 2u);
    EXPECT_EQ(diagnostics.plannedCrossQueueSyncCount, 1u);
    EXPECT_EQ(diagnostics.plannedQueueSyncCoveredCount, 0u);
    EXPECT_EQ(diagnostics.plannedQueueSyncUncoveredCount, 2u);
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
    EXPECT_EQ(diagnostics.plannedQueueSyncs[1].reason,
              RenderGraph::DiagnosticSyncReason::FinalQueueJoin);
    EXPECT_EQ(diagnostics.plannedQueueSyncs[1].sourceBatchIndex, 1u);
    EXPECT_EQ(diagnostics.plannedQueueSyncs[1].targetBatchIndex, 2u);
    EXPECT_EQ(diagnostics.actualQueueSyncCount, 0u);

    std::string dump = graph.ExportDiagnosticsText();
    EXPECT_NE(dump.find("Planned Queue Syncs"), std::string::npos);
    EXPECT_NE(dump.find("plannedSyncs=2"), std::string::npos);
    EXPECT_NE(dump.find("plannedCrossQueueSyncs=1"), std::string::npos);
    EXPECT_NE(dump.find("plannedUncoveredSyncs=2"), std::string::npos);
    EXPECT_NE(dump.find("[1] level=1 queue=Compute passes=[1] prerequisites=[0]"), std::string::npos);
    EXPECT_NE(dump.find("batch0 Graphics -> batch1 Compute reason=CrossQueueDependency"), std::string::npos);
}

TEST(RenderGraphValidation, SubmissionPlanExposesReusableReadyListPlan)
{
    FakeDevice device;
    device.MutableCapabilities().supportsAsyncCompute = true;
    device.MutableCapabilities().supportsExplicitQueueFenceSignal = true;
    device.MutableCapabilities().supportsQueueFenceWait = true;
    RenderGraph graph;
    graph.SetDevice(&device);
    ASSERT_TRUE(graph.SetQueueExecutionMode(
        RenderGraph::QueueExecutionMode::MultiQueue));

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
    ASSERT_EQ(plan.queueBatches.size(), 3u);
    EXPECT_EQ(plan.queueBatchCount, 3u);
    EXPECT_EQ(plan.dependencyLevelCount, 3u);
    EXPECT_EQ(plan.computeBatchCount, 1u);
    EXPECT_EQ(plan.asyncOverlapCandidateLevelCount, 0u);
    ASSERT_EQ(plan.queueSyncs.size(), 2u);
    EXPECT_EQ(plan.queueSyncCount, 2u);
    EXPECT_EQ(plan.crossQueueSyncCount, 1u);
    EXPECT_EQ(plan.queueBatches[1].prerequisiteSyncIndices, std::vector<uint32>({0u}));
    EXPECT_EQ(plan.queueSyncs[0].sourceBatchIndex, 0u);
    EXPECT_EQ(plan.queueSyncs[0].targetBatchIndex, 1u);
    EXPECT_EQ(plan.queueSyncs[0].sourcePassIndex, 0u);
    EXPECT_EQ(plan.queueSyncs[0].targetPassIndex, 1u);
    EXPECT_EQ(plan.terminalGraphicsBatchIndex, 2u);
    EXPECT_TRUE(plan.queueBatches[2].syntheticTerminal);
    EXPECT_EQ(plan.queueBatches[2].prerequisiteBatchIndices,
              std::vector<uint32>({1u}));

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
    ASSERT_TRUE(graph.SetQueueExecutionMode(
        RenderGraph::QueueExecutionMode::MultiQueue));

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
    ASSERT_EQ(plan.queueBatches.size(), 3u);
    EXPECT_EQ(plan.queueBatches[0].passIndices, std::vector<uint32>({0u, 1u}));
    EXPECT_EQ(plan.queueBatches[1].passIndices, std::vector<uint32>({2u, 3u}));
    EXPECT_EQ(plan.queueSyncCount, 2u);
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
    EXPECT_EQ(diagnostics.plannedQueueSyncCoveredCount, 2u);
    EXPECT_EQ(diagnostics.plannedQueueSyncUncoveredCount, 0u);
    EXPECT_EQ(diagnostics.actualMatchedPlannedSyncCount, 2u);
    EXPECT_EQ(diagnostics.actualUnplannedQueueSyncCount, 0u);
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
    ASSERT_TRUE(graph.SetQueueExecutionMode(
        RenderGraph::QueueExecutionMode::MultiQueue));

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
    ASSERT_EQ(diagnostics.plannedQueueSyncs.size(), 2u);
    ASSERT_EQ(diagnostics.queueSyncs.size(), 2u);
    EXPECT_EQ(diagnostics.plannedQueueSyncCoveredCount, 2u);
    EXPECT_EQ(diagnostics.plannedQueueSyncUncoveredCount, 0u);
    EXPECT_TRUE(diagnostics.plannedQueueSyncs[0].coveredByActualSync);
    EXPECT_EQ(diagnostics.plannedQueueSyncs[0].actualSyncIndex, 0u);

    EXPECT_EQ(diagnostics.actualMatchedPlannedSyncCount, 2u);
    EXPECT_EQ(diagnostics.actualUnplannedQueueSyncCount, 0u);
    EXPECT_EQ(diagnostics.actualConservativeFinalJoinCount, 1u);
    EXPECT_TRUE(diagnostics.queueSyncs[0].coversPlannedSync);
    EXPECT_EQ(diagnostics.queueSyncs[0].plannedSyncIndex, 0u);
    EXPECT_TRUE(diagnostics.queueSyncs[1].coversPlannedSync);
    EXPECT_EQ(diagnostics.queueSyncs[1].plannedSyncIndex, 1u);
    EXPECT_EQ(diagnostics.queueSyncs[1].reason, RenderGraph::DiagnosticSyncReason::FinalQueueJoin);

    std::string dump = graph.ExportDiagnosticsText();
    EXPECT_NE(dump.find("plannedCoveredSyncs=2"), std::string::npos);
    EXPECT_NE(dump.find("plannedUncoveredSyncs=0"), std::string::npos);
    EXPECT_NE(dump.find("actualMatchedPlannedSyncs=2"), std::string::npos);
    EXPECT_NE(dump.find("actualUnplannedSyncs=0"), std::string::npos);
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
    ASSERT_TRUE(graph.SetQueueExecutionMode(
        RenderGraph::QueueExecutionMode::MultiQueue));

    RHIBufferDesc graphicsBufferDesc;
    graphicsBufferDesc.size = 1024;
    graphicsBufferDesc.usage = RHIBufferUsage::Structured | RHIBufferUsage::UnorderedAccess;
    FakeBuffer graphicsBuffer(graphicsBufferDesc);

    RHIBufferDesc computeBufferDesc;
    computeBufferDesc.size = 1024;
    computeBufferDesc.usage = RHIBufferUsage::Structured | RHIBufferUsage::UnorderedAccess;
    FakeBuffer computeBuffer(computeBufferDesc);

    RGBufferHandle graphicsResource = graph.ImportBuffer(&graphicsBuffer, RHIResourceState::Common);
    RGBufferHandle computeResource = graph.ImportBuffer(
        &computeBuffer,
        MakeRHIBufferAccessSnapshot(
            RHIResourceState::Common,
            RHIShaderStage::None,
            GPUQueueDomain::Compute,
            RHIContentValidity::Valid));

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
    EXPECT_EQ(plannedOnly.plannedQueueBatchCount, 4u);
    EXPECT_EQ(plannedOnly.plannedDependencyLevelCount, 3u);
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
    EXPECT_EQ(executed.plannedQueueBatchCount, 4u);
    EXPECT_EQ(executed.plannedAsyncOverlapCandidateLevelCount, 1u);
    EXPECT_EQ(executed.actualQueueBatchCount, 3u);
    EXPECT_EQ(executed.actualQueueSwitchCount, 2u);
    EXPECT_EQ(executed.actualQueueSyncCount, 1u);
    EXPECT_EQ(executed.actualCrossQueueSyncCount, 0u);

    std::string dump = graph.ExportDiagnosticsText();
    EXPECT_NE(dump.find("Schedule efficiency"), std::string::npos);
    EXPECT_NE(dump.find("plannedBatches=4"), std::string::npos);
    EXPECT_NE(dump.find("plannedLevels=3"), std::string::npos);
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
    ASSERT_TRUE(graph.SetQueueExecutionMode(
        RenderGraph::QueueExecutionMode::MultiQueue));

    RHIBufferDesc graphicsBufferDesc;
    graphicsBufferDesc.size = 1024;
    graphicsBufferDesc.usage = RHIBufferUsage::Structured | RHIBufferUsage::UnorderedAccess;
    FakeBuffer graphicsBuffer(graphicsBufferDesc);

    RHIBufferDesc computeBufferDesc;
    computeBufferDesc.size = 1024;
    computeBufferDesc.usage = RHIBufferUsage::Structured | RHIBufferUsage::UnorderedAccess;
    FakeBuffer computeBuffer(computeBufferDesc);

    RGBufferHandle graphicsResource = graph.ImportBuffer(&graphicsBuffer, RHIResourceState::Common);
    RGBufferHandle computeResource = graph.ImportBuffer(
        &computeBuffer,
        MakeRHIBufferAccessSnapshot(
            RHIResourceState::Common,
            RHIShaderStage::None,
            GPUQueueDomain::Compute,
            RHIContentValidity::Valid));

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

    const RenderGraph::SubmissionPlan independentPlan =
        graph.GetSubmissionPlan();
    EXPECT_EQ(independentPlan.queueBatches.size(), 4u);
    EXPECT_EQ(independentPlan.crossQueueSyncCount, 0u);

    graph.ExecuteAsync(graphicsCtx, &computeCtx, &computeFence, 11);

    const auto& stats = graph.GetCompileStats();
    SCOPED_TRACE(graph.ExportDiagnosticsText());
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

TEST(RenderGraphValidation, MultiQueueRecordsDistinctContextsForGraphicsComputeGraphics)
{
    FakeDevice device;
    auto& capabilities = device.MutableCapabilities();
    capabilities.supportsAsyncCompute = true;
    capabilities.supportsDefaultQueueFenceSignal = true;
    capabilities.supportsQueueSubmissionPlan = true;
    capabilities.queueTopology.logicalQueueDomains = {
        GPUQueueDomain::Graphics,
        GPUQueueDomain::Compute,
        GPUQueueDomain::Copy};
    capabilities.queueTopology.activeDomainCount = 3;

    RenderGraph graph;
    graph.SetDevice(&device);
    ASSERT_TRUE(graph.SetQueueExecutionMode(
        RenderGraph::QueueExecutionMode::MultiQueue));
    RHIBufferDesc desc;
    desc.size = 1024;
    desc.usage = RHIBufferUsage::Structured |
                 RHIBufferUsage::UnorderedAccess;
    FakeBuffer buffer(desc);
    const RGBufferHandle resource = graph.ImportBuffer(
        &buffer, RHIResourceState::Common);
    struct Data { RGBufferHandle buffer; };
    graph.AddPass<Data>(
        "QueuePlanGraphicsProduce",
        RenderGraphPassType::Graphics,
        [resource](RenderGraphBuilder& builder, Data& data)
        {
            data.buffer = builder.Write(
                resource, RHIResourceState::UnorderedAccess);
        },
        [](const Data&, RHICommandContext&) {});
    graph.AddPass<Data>(
        "QueuePlanComputeTransform",
        RenderGraphPassType::Compute,
        [resource](RenderGraphBuilder& builder, Data& data)
        {
            data.buffer = builder.ReadWrite(resource);
        },
        [](const Data&, RHICommandContext&) {});
    graph.AddPass<Data>(
        "QueuePlanGraphicsConsume",
        RenderGraphPassType::Graphics,
        [resource](RenderGraphBuilder& builder, Data& data)
        {
            data.buffer = builder.ReadWrite(resource);
        },
        [](const Data&, RHICommandContext&) {});
    graph.SetExportState(resource, RHIResourceState::ShaderResource);
    graph.Compile();
    ASSERT_TRUE(graph.GetCompileStats().compileValid);

    RenderGraph::RecordedQueueSubmission recorded;
    ASSERT_TRUE(graph.RecordQueueSubmission(recorded));
    ASSERT_TRUE(ValidateRHIQueueSubmissionPlan(recorded.plan));
    ASSERT_EQ(recorded.plan.batches.size(), 4u);
    ASSERT_EQ(recorded.ownedContexts.size(), 4u);
    EXPECT_EQ(recorded.plan.terminalGraphicsBatchIndex, 3u);
    EXPECT_EQ(recorded.plan.batches[0].queueType,
              RHICommandQueueType::Graphics);
    EXPECT_EQ(recorded.plan.batches[1].queueType,
              RHICommandQueueType::Compute);
    EXPECT_EQ(recorded.plan.batches[2].queueType,
              RHICommandQueueType::Graphics);
    EXPECT_EQ(recorded.plan.batches[3].queueType,
              RHICommandQueueType::Graphics);
    EXPECT_NE(recorded.ownedContexts[0].Get(),
              recorded.ownedContexts[2].Get());

    const auto hasOwnership = [](const FakeCommandContext& context,
                                 GPUQueueDomain before,
                                 GPUQueueDomain after)
    {
        return std::any_of(
            context.bufferBarriers.begin(),
            context.bufferBarriers.end(),
            [before, after](const RHIBufferBarrier& barrier)
            {
                return barrier.hasScopedAccess &&
                       barrier.accessBefore.domain == before &&
                       barrier.accessAfter.domain == after &&
                       HasDependencyKind(
                           barrier.dependencyKind,
                           RHIDependencyKind::Ownership);
            });
    };
    const auto& graphicsProduce = *static_cast<FakeCommandContext*>(
        recorded.ownedContexts[0].Get());
    const auto& computeTransform = *static_cast<FakeCommandContext*>(
        recorded.ownedContexts[1].Get());
    const auto& graphicsConsume = *static_cast<FakeCommandContext*>(
        recorded.ownedContexts[2].Get());
    EXPECT_TRUE(hasOwnership(graphicsProduce,
                             GPUQueueDomain::Graphics,
                             GPUQueueDomain::Compute));
    EXPECT_TRUE(hasOwnership(computeTransform,
                             GPUQueueDomain::Graphics,
                             GPUQueueDomain::Compute));
    EXPECT_TRUE(hasOwnership(computeTransform,
                             GPUQueueDomain::Compute,
                             GPUQueueDomain::Graphics));
    EXPECT_TRUE(hasOwnership(graphicsConsume,
                             GPUQueueDomain::Compute,
                             GPUQueueDomain::Graphics));
}

TEST(RenderGraphValidation,
     ParallelRecordingRunsIndependentDependencyLevelBatchesConcurrently)
{
    ScopedJobSystem jobs(2);
    FakeDevice device;
    auto& capabilities = device.MutableCapabilities();
    capabilities.supportsAsyncCompute = true;
    capabilities.supportsDefaultQueueFenceSignal = true;
    capabilities.supportsQueueSubmissionPlan = true;
    capabilities.queueTopology.logicalQueueDomains = {
        GPUQueueDomain::Graphics,
        GPUQueueDomain::Compute,
        GPUQueueDomain::Copy};
    capabilities.queueTopology.activeDomainCount = 3;

    RenderGraph graph;
    graph.SetDevice(&device);
    ASSERT_TRUE(graph.SetQueueExecutionMode(
        RenderGraph::QueueExecutionMode::MultiQueue));
    graph.SetParallelRecordingEnabled(true);
    ASSERT_TRUE(graph.IsParallelRecordingEnabled());

    RHIBufferDesc desc;
    desc.size = 256;
    desc.usage = RHIBufferUsage::Structured |
                 RHIBufferUsage::UnorderedAccess;
    FakeBuffer graphicsBuffer(desc);
    FakeBuffer computeBuffer(desc);
    const RGBufferHandle graphicsResource = graph.ImportBuffer(
        &graphicsBuffer, RHIResourceState::Common);
    const RGBufferHandle computeResource = graph.ImportBuffer(
        &computeBuffer, RHIResourceState::Common);

    std::mutex rendezvousMutex;
    std::condition_variable rendezvousCv;
    uint32 arrived = 0;
    std::atomic<uint32> successfulRendezvous = 0;
    const auto rendezvous = [&]()
    {
        std::unique_lock lock(rendezvousMutex);
        ++arrived;
        rendezvousCv.notify_all();
        if (rendezvousCv.wait_for(
                lock,
                std::chrono::seconds(2),
                [&arrived]() { return arrived == 2U; }))
        {
            successfulRendezvous.fetch_add(1, std::memory_order_relaxed);
        }
    };

    struct Data
    {
        RGBufferHandle buffer;
    };
    graph.AddPass<Data>(
        "ParallelGraphics",
        RenderGraphPassType::Graphics,
        [graphicsResource](RenderGraphBuilder& builder, Data& data)
        {
            data.buffer = builder.Write(
                graphicsResource, RHIResourceState::UnorderedAccess);
        },
        [&rendezvous](const Data&, RHICommandContext&) { rendezvous(); });
    graph.AddPass<Data>(
        "ParallelCompute",
        RenderGraphPassType::Compute,
        [computeResource](RenderGraphBuilder& builder, Data& data)
        {
            data.buffer = builder.Write(
                computeResource, RHIResourceState::UnorderedAccess);
        },
        [&rendezvous](const Data&, RHICommandContext&) { rendezvous(); });
    graph.SetExportState(graphicsResource, RHIResourceState::ShaderResource);
    graph.SetExportState(computeResource, RHIResourceState::ShaderResource);
    graph.Compile();
    ASSERT_TRUE(graph.GetCompileStats().compileValid);

    const RenderGraph::SubmissionPlan plan = graph.GetSubmissionPlan();
    ASSERT_GE(plan.queueBatches.size(), 3U);
    ASSERT_GE(plan.asyncOverlapCandidateLevelCount, 1U);

    RenderGraph::RecordedQueueSubmission recorded;
    ASSERT_TRUE(graph.RecordQueueSubmission(recorded));
    EXPECT_EQ(successfulRendezvous.load(std::memory_order_relaxed), 2U);
    const RenderGraph::CompileStats& stats = graph.GetCompileStats();
    EXPECT_TRUE(stats.parallelRecordingEnabled);
    EXPECT_TRUE(stats.parallelRecordingUsed);
    EXPECT_EQ(stats.lastParallelRecordingLevelCount, 1U);
    EXPECT_EQ(stats.lastParallelRecordingBatchCount, 2U);
    EXPECT_EQ(stats.lastExecutedPassCount, 2U);

    const RenderGraph::Diagnostics diagnostics = graph.GetDiagnostics();
    ASSERT_EQ(diagnostics.passes.size(), 2U);
    EXPECT_EQ(diagnostics.passes[0].executionSerial, 0U);
    EXPECT_EQ(diagnostics.passes[1].executionSerial, 1U);
    const std::string json = graph.ExportDiagnosticsJson();
    EXPECT_NE(json.find("\"parallelRecordingUsed\": true"),
              std::string::npos);
}

TEST(RenderGraphValidation, MultiQueuePlansCopyComputeGraphicsAndTerminalJoin)
{
    FakeDevice device;
    auto& capabilities = device.MutableCapabilities();
    capabilities.supportsAsyncCompute = true;
    capabilities.supportsDefaultQueueFenceSignal = true;
    capabilities.supportsQueueSubmissionPlan = true;
    capabilities.queueTopology.logicalQueueDomains = {
        GPUQueueDomain::Graphics,
        GPUQueueDomain::Compute,
        GPUQueueDomain::Copy};
    capabilities.queueTopology.activeDomainCount = 3;
    RenderGraph graph;
    graph.SetDevice(&device);
    ASSERT_TRUE(graph.SetQueueExecutionMode(
        RenderGraph::QueueExecutionMode::MultiQueue));
    RHIBufferDesc desc;
    desc.size = 2048;
    desc.usage = RHIBufferUsage::CopyDst |
                 RHIBufferUsage::Structured |
                 RHIBufferUsage::UnorderedAccess;
    FakeBuffer buffer(desc);
    const RGBufferHandle resource = graph.ImportBuffer(
        &buffer, RHIResourceState::Common);
    struct Data { RGBufferHandle buffer; };
    graph.AddPass<Data>(
        "QueuePlanCopy",
        RenderGraphPassType::Copy,
        [resource](RenderGraphBuilder& builder, Data& data)
        {
            data.buffer = builder.Write(resource, RHIResourceState::CopyDest);
        },
        [](const Data&, RHICommandContext&) {});
    graph.AddPass<Data>(
        "QueuePlanCompute",
        RenderGraphPassType::Compute,
        [resource](RenderGraphBuilder& builder, Data& data)
        {
            data.buffer = builder.ReadWrite(resource);
        },
        [](const Data&, RHICommandContext&) {});
    graph.AddPass<Data>(
        "QueuePlanGraphics",
        RenderGraphPassType::Graphics,
        [resource](RenderGraphBuilder& builder, Data& data)
        {
            data.buffer = builder.ReadWrite(resource);
        },
        [](const Data&, RHICommandContext&) {});
    graph.SetExportState(resource, RHIResourceState::ShaderResource);
    graph.Compile();
    ASSERT_TRUE(graph.GetCompileStats().compileValid);

    const RenderGraph::SubmissionPlan plan = graph.GetSubmissionPlan();
    ASSERT_EQ(plan.queueBatches.size(), 5u);
    EXPECT_EQ(plan.copyBatchCount, 1u);
    EXPECT_EQ(plan.computeBatchCount, 1u);
    EXPECT_EQ(plan.queueBatches[0].queue,
              RenderGraph::DiagnosticExecutionQueue::Graphics);
    EXPECT_TRUE(plan.queueBatches[0].syntheticInitialRelease);
    EXPECT_EQ(plan.queueBatches[1].queue,
              RenderGraph::DiagnosticExecutionQueue::Copy);
    EXPECT_EQ(plan.queueBatches[2].queue,
              RenderGraph::DiagnosticExecutionQueue::Compute);
    EXPECT_EQ(plan.queueBatches[3].queue,
              RenderGraph::DiagnosticExecutionQueue::Graphics);
    EXPECT_TRUE(plan.queueBatches[4].syntheticTerminal);
    EXPECT_EQ(plan.queueBatches[1].prerequisiteBatchIndices,
              std::vector<uint32>({0u}));
    EXPECT_EQ(plan.queueBatches[2].prerequisiteBatchIndices,
              std::vector<uint32>({1u}));

    RenderGraph::RecordedQueueSubmission recorded;
    ASSERT_TRUE(graph.RecordQueueSubmission(recorded));
    EXPECT_TRUE(ValidateRHIQueueSubmissionPlan(recorded.plan));
    ASSERT_EQ(recorded.ownedContexts.size(), 5u);
    const auto& initialRelease = *static_cast<FakeCommandContext*>(
        recorded.ownedContexts[0].Get());
    ASSERT_FALSE(initialRelease.bufferBarriers.empty());
    EXPECT_TRUE(std::any_of(
        initialRelease.bufferBarriers.begin(),
        initialRelease.bufferBarriers.end(),
        [](const RHIBufferBarrier& barrier)
        {
            return barrier.hasScopedAccess &&
                   barrier.accessBefore.domain == GPUQueueDomain::Graphics &&
                   barrier.accessAfter.domain == GPUQueueDomain::Copy &&
                   HasDependencyKind(barrier.dependencyKind,
                                     RHIDependencyKind::Ownership);
        }));
}

TEST(RenderGraphValidation,
     MultiQueueExportReleasesUntouchedExternalRangesToTerminal)
{
    FakeDevice device;
    auto& capabilities = device.MutableCapabilities();
    capabilities.supportsAsyncCompute = true;
    capabilities.supportsDefaultQueueFenceSignal = true;
    capabilities.supportsQueueSubmissionPlan = true;
    capabilities.queueTopology.logicalQueueDomains = {
        GPUQueueDomain::Graphics,
        GPUQueueDomain::Compute,
        GPUQueueDomain::Copy};
    capabilities.queueTopology.activeDomainCount = 3;

    RenderGraph graph;
    graph.SetDevice(&device);
    ASSERT_TRUE(graph.SetQueueExecutionMode(
        RenderGraph::QueueExecutionMode::MultiQueue));

    constexpr uint64 bufferSize = 384;
    constexpr uint64 dirtyOffset = 96;
    constexpr uint64 dirtySize = 192;
    RHIBufferDesc desc;
    desc.size = bufferSize;
    desc.usage = RHIBufferUsage::CopyDst |
                 RHIBufferUsage::Structured;
    FakeBuffer buffer(desc);
    const RGBufferHandle resource = graph.ImportBuffer(
        &buffer,
        MakeRHIBufferAccessSnapshot(
            RHIResourceState::ShaderResource,
            RHIShaderStage::Compute,
            GPUQueueDomain::Compute,
            RHIContentValidity::Valid));

    struct Data
    {
        RGBufferHandle buffer;
    };
    graph.AddPass<Data>(
        "PartialCopyUpdate",
        RenderGraphPassType::Copy,
        [resource](RenderGraphBuilder& builder, Data& data)
        {
            data.buffer = builder.Write(
                resource.Range(dirtyOffset, dirtySize),
                RHIResourceState::CopyDest);
        },
        [](const Data&, RHICommandContext&) {});
    graph.SetExportAccess(
        resource,
        MakeRHIAccessSnapshot(
            RHIResourceState::ShaderResource,
            RHIShaderStage::AllGraphics,
            GPUQueueDomain::Graphics,
            RHIContentValidity::Valid));
    graph.Compile();
    ASSERT_TRUE(graph.GetCompileStats().compileValid);

    const RenderGraph::SubmissionPlan plan = graph.GetSubmissionPlan();
    const auto initialRelease = std::find_if(
        plan.queueBatches.begin(),
        plan.queueBatches.end(),
        [](const RenderGraph::PlannedQueueBatchDiagnostic& batch)
        {
            return batch.syntheticInitialRelease &&
                   batch.queue ==
                       RenderGraph::DiagnosticExecutionQueue::Compute;
        });
    ASSERT_NE(initialRelease, plan.queueBatches.end());
    ASSERT_NE(plan.terminalGraphicsBatchIndex, RVX_INVALID_INDEX);
    ASSERT_LT(plan.terminalGraphicsBatchIndex, plan.queueBatches.size());
    const auto& terminal =
        plan.queueBatches[plan.terminalGraphicsBatchIndex];
    EXPECT_NE(std::find(terminal.prerequisiteBatchIndices.begin(),
                        terminal.prerequisiteBatchIndices.end(),
                        initialRelease->batchIndex),
              terminal.prerequisiteBatchIndices.end());

    RenderGraph::RecordedQueueSubmission recorded;
    ASSERT_TRUE(graph.RecordQueueSubmission(recorded));
    ASSERT_TRUE(ValidateRHIQueueSubmissionPlan(recorded.plan));
    ASSERT_EQ(recorded.ownedContexts.size(), plan.queueBatches.size());

    const auto& releaseContext = *static_cast<FakeCommandContext*>(
        recorded.ownedContexts[initialRelease->batchIndex].Get());
    const auto& terminalContext = *static_cast<FakeCommandContext*>(
        recorded.ownedContexts[plan.terminalGraphicsBatchIndex].Get());
    const auto hasOwnershipRange = [](
        const FakeCommandContext& context,
        GPUQueueDomain before,
        GPUQueueDomain after,
        uint64 offset,
        uint64 size)
    {
        return std::any_of(
            context.bufferBarriers.begin(),
            context.bufferBarriers.end(),
            [before, after, offset, size](const RHIBufferBarrier& barrier)
            {
                return barrier.hasScopedAccess &&
                       barrier.accessBefore.domain == before &&
                       barrier.accessAfter.domain == after &&
                       barrier.offset == offset &&
                       barrier.size == size &&
                       HasDependencyKind(
                           barrier.dependencyKind,
                           RHIDependencyKind::Ownership);
            });
    };

    EXPECT_TRUE(hasOwnershipRange(
        releaseContext,
        GPUQueueDomain::Compute,
        GPUQueueDomain::Copy,
        dirtyOffset,
        dirtySize));
    EXPECT_TRUE(hasOwnershipRange(
        releaseContext,
        GPUQueueDomain::Compute,
        GPUQueueDomain::Graphics,
        0,
        dirtyOffset));
    EXPECT_TRUE(hasOwnershipRange(
        releaseContext,
        GPUQueueDomain::Compute,
        GPUQueueDomain::Graphics,
        dirtyOffset + dirtySize,
        bufferSize - dirtyOffset - dirtySize));
    EXPECT_TRUE(hasOwnershipRange(
        terminalContext,
        GPUQueueDomain::Compute,
        GPUQueueDomain::Graphics,
        0,
        dirtyOffset));
    EXPECT_TRUE(hasOwnershipRange(
        terminalContext,
        GPUQueueDomain::Copy,
        GPUQueueDomain::Graphics,
        dirtyOffset,
        dirtySize));
    EXPECT_TRUE(hasOwnershipRange(
        terminalContext,
        GPUQueueDomain::Compute,
        GPUQueueDomain::Graphics,
        dirtyOffset + dirtySize,
        bufferSize - dirtyOffset - dirtySize));
}

TEST(RenderGraphValidation, MultiQueueTerminalJoinsIndependentBranchesAndFoldsAliases)
{
    FakeDevice device;
    auto& capabilities = device.MutableCapabilities();
    capabilities.supportsAsyncCompute = true;
    capabilities.supportsDefaultQueueFenceSignal = true;
    capabilities.supportsQueueSubmissionPlan = true;
    capabilities.queueTopology.logicalQueueDomains = {
        GPUQueueDomain::Graphics,
        GPUQueueDomain::Compute,
        GPUQueueDomain::Graphics};
    capabilities.queueTopology.activeDomainCount = 2;
    RenderGraph graph;
    graph.SetDevice(&device);
    ASSERT_TRUE(graph.SetQueueExecutionMode(
        RenderGraph::QueueExecutionMode::MultiQueue));
    RHIBufferDesc desc;
    desc.size = 512;
    desc.usage = RHIBufferUsage::Structured |
                 RHIBufferUsage::UnorderedAccess;
    FakeBuffer graphicsBuffer(desc);
    FakeBuffer computeBuffer(desc);
    const RGBufferHandle graphicsResource = graph.ImportBuffer(
        &graphicsBuffer, RHIResourceState::Common);
    const RGBufferHandle computeResource = graph.ImportBuffer(
        &computeBuffer, RHIResourceState::Common);
    struct Data { RGBufferHandle buffer; };
    graph.AddPass<Data>(
        "IndependentGraphics",
        RenderGraphPassType::Graphics,
        [graphicsResource](RenderGraphBuilder& builder, Data& data)
        {
            data.buffer = builder.Write(
                graphicsResource, RHIResourceState::UnorderedAccess);
        },
        [](const Data&, RHICommandContext&) {});
    graph.AddPass<Data>(
        "IndependentCompute",
        RenderGraphPassType::Compute,
        [computeResource](RenderGraphBuilder& builder, Data& data)
        {
            data.buffer = builder.Write(
                computeResource, RHIResourceState::UnorderedAccess);
        },
        [](const Data&, RHICommandContext&) {});
    graph.SetExportState(graphicsResource, RHIResourceState::ShaderResource);
    graph.SetExportState(computeResource, RHIResourceState::ShaderResource);
    graph.Compile();
    ASSERT_TRUE(graph.GetCompileStats().compileValid);
    RenderGraph::RecordedQueueSubmission recorded;
    ASSERT_TRUE(graph.RecordQueueSubmission(recorded));
    ASSERT_TRUE(ValidateRHIQueueSubmissionPlan(recorded.plan));
    const auto& terminal = recorded.plan.batches[
        recorded.plan.terminalGraphicsBatchIndex];
    EXPECT_EQ(terminal.prerequisiteBatchIndices.size(), 2u);

    capabilities.queueTopology.logicalQueueDomains[1] =
        GPUQueueDomain::Graphics;
    capabilities.queueTopology.activeDomainCount = 1;
    RenderGraph aliasedGraph;
    aliasedGraph.SetDevice(&device);
    ASSERT_TRUE(aliasedGraph.SetQueueExecutionMode(
        RenderGraph::QueueExecutionMode::MultiQueue));
    FakeBuffer aliasedBuffer(desc);
    const RGBufferHandle aliasedResource = aliasedGraph.ImportBuffer(
        &aliasedBuffer, RHIResourceState::Common);
    aliasedGraph.AddPass<Data>(
        "AliasedCompute",
        RenderGraphPassType::Compute,
        [aliasedResource](RenderGraphBuilder& builder, Data& data)
        {
            data.buffer = builder.Write(
                aliasedResource, RHIResourceState::UnorderedAccess);
        },
        [](const Data&, RHICommandContext&) {});
    aliasedGraph.SetExportState(
        aliasedResource, RHIResourceState::ShaderResource);
    aliasedGraph.Compile();
    const RenderGraph::SubmissionPlan aliasedPlan =
        aliasedGraph.GetSubmissionPlan();
    EXPECT_EQ(aliasedPlan.computeBatchCount, 0u);
    EXPECT_EQ(aliasedPlan.queueSyncCount, 0u);
    EXPECT_TRUE(std::all_of(
        aliasedPlan.queueBatches.begin(),
        aliasedPlan.queueBatches.end(),
        [](const RenderGraph::PlannedQueueBatchDiagnostic& batch)
        {
            return batch.queue ==
                RenderGraph::DiagnosticExecutionQueue::Graphics;
        }));
}

TEST(RenderGraphValidation, GraphicsOnlyFallbackRejectsExternalNonGraphicsOwnership)
{
    FakeDevice device;
    RenderGraph graph;
    graph.SetDevice(&device);
    ASSERT_TRUE(graph.SetQueueExecutionMode(
        RenderGraph::QueueExecutionMode::MultiQueue));

    RHIBufferDesc desc;
    desc.size = 256;
    desc.usage = RHIBufferUsage::CopyDst |
                 RHIBufferUsage::Structured;
    FakeBuffer buffer(desc);
    const RGBufferHandle resource = graph.ImportBuffer(
        &buffer,
        MakeRHIBufferAccessSnapshot(
            RHIResourceState::CopySource,
            RHIShaderStage::None,
            GPUQueueDomain::Copy,
            RHIContentValidity::Valid));
    struct Data { RGBufferHandle buffer; };
    graph.AddPass<Data>(
        "CopyOwnedInput",
        RenderGraphPassType::Copy,
        [resource](RenderGraphBuilder& builder, Data& data)
        {
            data.buffer = builder.Write(
                resource, RHIResourceState::CopyDest);
        },
        [](const Data&, RHICommandContext&) {});
    graph.SetExportAccess(
        resource,
        MakeRHIAccessSnapshot(
            RHIResourceState::ShaderResource,
            RHIShaderStage::All,
            GPUQueueDomain::Graphics,
            RHIContentValidity::Valid));
    graph.Compile();
    EXPECT_FALSE(graph.GetCompileStats().compileValid);
    EXPECT_GT(graph.GetCompileStats().validationErrorCount, 0u);
}

TEST(RenderGraphValidation, GraphicsOnlyFallbackRebuildsAllQueueSemantics)
{
    FakeDevice device;
    auto& capabilities = device.MutableCapabilities();
    capabilities.supportsDefaultQueueFenceSignal = true;
    capabilities.supportsQueueSubmissionPlan = true;
    capabilities.queueTopology.logicalQueueDomains = {
        GPUQueueDomain::Graphics,
        GPUQueueDomain::Compute,
        GPUQueueDomain::Copy};
    capabilities.queueTopology.activeDomainCount = 3;

    RenderGraph graph;
    graph.SetDevice(&device);
    ASSERT_TRUE(graph.SetQueueExecutionMode(
        RenderGraph::QueueExecutionMode::MultiQueue));
    RHIBufferDesc desc;
    desc.size = 256;
    desc.usage = RHIBufferUsage::CopyDst |
                 RHIBufferUsage::Structured;
    FakeBuffer buffer(desc);
    const RGBufferHandle resource = graph.ImportBuffer(
        &buffer,
        MakeRHIBufferAccessSnapshot(
            RHIResourceState::Common,
            RHIShaderStage::None,
            GPUQueueDomain::Graphics,
            RHIContentValidity::Valid));
    struct Data { RGBufferHandle buffer; };
    graph.AddPass<Data>(
        "FallbackCopy",
        RenderGraphPassType::Copy,
        [resource](RenderGraphBuilder& builder, Data& data)
        {
            data.buffer = builder.Write(
                resource, RHIResourceState::CopyDest);
        },
        [](const Data&, RHICommandContext&) {});
    graph.SetExportAccess(
        resource,
        MakeRHIAccessSnapshot(
            RHIResourceState::ShaderResource,
            RHIShaderStage::All,
            GPUQueueDomain::Graphics,
            RHIContentValidity::Valid));
    graph.Compile();
    ASSERT_TRUE(graph.GetCompileStats().compileValid);
    EXPECT_GT(graph.GetSubmissionPlan().copyBatchCount, 0u);

    ASSERT_TRUE(graph.RecompileGraphicsOnly());
    EXPECT_EQ(graph.GetQueueExecutionMode(),
              RenderGraph::QueueExecutionMode::GraphicsOnly);
    const RenderGraph::SubmissionPlan fallbackPlan =
        graph.GetSubmissionPlan();
    EXPECT_EQ(fallbackPlan.computeBatchCount, 0u);
    EXPECT_EQ(fallbackPlan.copyBatchCount, 0u);
    EXPECT_TRUE(std::all_of(
        fallbackPlan.queueBatches.begin(),
        fallbackPlan.queueBatches.end(),
        [](const RenderGraph::PlannedQueueBatchDiagnostic& batch)
        {
            return batch.queue ==
                RenderGraph::DiagnosticExecutionQueue::Graphics;
        }));

    FakeCommandContext graphicsContext;
    graph.Execute(graphicsContext);
    EXPECT_EQ(graph.GetCompileStats().executionQueueMismatchCount, 0u);
    EXPECT_TRUE(std::all_of(
        graphicsContext.bufferBarriers.begin(),
        graphicsContext.bufferBarriers.end(),
        [](const RHIBufferBarrier& barrier)
        {
            return barrier.accessBefore.domain ==
                       GPUQueueDomain::Graphics &&
                   barrier.accessAfter.domain ==
                       GPUQueueDomain::Graphics &&
                   !HasDependencyKind(
                       barrier.dependencyKind,
                       RHIDependencyKind::Ownership);
        }));
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
    RGTextureHandle invalidTexture;
    invalidTexture.index = 777;
    invalidTexture.graphIdentity = graph.GetGraphIdentity();
    invalidTexture.recordingGeneration = graph.GetRecordingGeneration();

    graph.AddPass<SimplePassData>(
        "InvalidTextureUsage",
        RenderGraphPassType::Graphics,
        [invalidTexture](RenderGraphBuilder& builder, SimplePassData& data)
        {
            data.colorTarget = builder.Write(invalidTexture, RHIResourceState::RenderTarget);
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

TEST(RenderGraphValidation, ForeignHandlesAreRejectedBeforePassUsageRecording)
{
    RenderGraph sourceGraph;
    RenderGraph destinationGraph;
    EXPECT_NE(sourceGraph.GetGraphIdentity(), 0u);
    EXPECT_NE(destinationGraph.GetGraphIdentity(), 0u);
    EXPECT_NE(sourceGraph.GetGraphIdentity(), destinationGraph.GetGraphIdentity());
    EXPECT_EQ(sourceGraph.GetRecordingGeneration(), 1u);
    EXPECT_EQ(destinationGraph.GetRecordingGeneration(), 1u);

    const RHITextureDesc textureDesc =
        RHITextureDesc::RenderTarget(32, 32, RHIFormat::RGBA16_FLOAT);
    RHIBufferDesc bufferDesc;
    bufferDesc.size = 64;
    bufferDesc.usage = RHIBufferUsage::ShaderResource;

    const RGTextureHandle foreignTexture = sourceGraph.CreateTexture(textureDesc);
    const RGBufferHandle foreignBuffer = sourceGraph.CreateBuffer(bufferDesc);
    const RGTextureHandle localTexture = destinationGraph.CreateTexture(textureDesc);
    const RGBufferHandle localBuffer = destinationGraph.CreateBuffer(bufferDesc);
    bool foreignTextureRejected = false;
    bool foreignBufferRejected = false;
    bool localTextureAccepted = false;
    bool localBufferAccepted = false;

    struct ProvenancePassData
    {
        RGTextureHandle foreignTexture;
        RGBufferHandle foreignBuffer;
        RGTextureHandle localTexture;
        RGBufferHandle localBuffer;
    };
    destinationGraph.AddPass<ProvenancePassData>(
        "RejectForeignProvenance",
        RenderGraphPassType::Graphics,
        [foreignTexture,
         foreignBuffer,
         localTexture,
         localBuffer,
         &foreignTextureRejected,
         &foreignBufferRejected,
         &localTextureAccepted,
         &localBufferAccepted](
            RenderGraphBuilder& builder,
            ProvenancePassData& data)
        {
            data.foreignTexture = builder.Write(
                foreignTexture, RHIResourceState::RenderTarget);
            data.foreignBuffer = builder.Read(
                foreignBuffer, RHIShaderStage::Vertex);
            builder.SetDepthStencil(foreignTexture);
            builder.Read(RGTextureHandle{});
            builder.Write(RGBufferHandle{});
            data.localTexture = builder.Write(
                localTexture, RHIResourceState::RenderTarget);
            data.localBuffer = builder.Write(
                localBuffer, RHIResourceState::UnorderedAccess);
            foreignTextureRejected = !data.foreignTexture.IsValid();
            foreignBufferRejected = !data.foreignBuffer.IsValid();
            localTextureAccepted = data.localTexture.IsValid();
            localBufferAccepted = data.localBuffer.IsValid();
        },
        [](const ProvenancePassData&, RHICommandContext&) {});

    EXPECT_TRUE(foreignTextureRejected);
    EXPECT_TRUE(foreignBufferRejected);
    EXPECT_TRUE(localTextureAccepted);
    EXPECT_TRUE(localBufferAccepted);

    EXPECT_EQ(destinationGraph.GetTextureDesc(foreignTexture), nullptr);
    EXPECT_EQ(destinationGraph.GetBufferDesc(foreignBuffer), nullptr);
    EXPECT_EQ(destinationGraph.GetRealizedAccess(foreignTexture).uniformAccess.layout,
              RHIResourceLayout::Undefined);
    EXPECT_EQ(destinationGraph.GetRealizedAccess(foreignBuffer).uniformAccess.layout,
              RHIResourceLayout::Undefined);

    destinationGraph.SetExportState(foreignTexture, RHIResourceState::Present);
    destinationGraph.SetExportState(localTexture, RHIResourceState::Present);
    destinationGraph.Compile();
    EXPECT_FALSE(destinationGraph.GetCompileStats().compileValid);
    EXPECT_EQ(destinationGraph.GetCompileStats().invalidResourceUsageCount, 3u);
    ASSERT_FALSE(destinationGraph.GetCompileDiagnostics().empty());
    EXPECT_NE(destinationGraph.GetCompileDiagnostics().front().find("invalid"),
              std::string::npos);
}

TEST(RenderGraphValidation, ClearInvalidatesOldHandleGenerationWithoutChangingGraphIdentity)
{
    RenderGraph graph;
    const uint64 graphIdentity = graph.GetGraphIdentity();
    const uint64 firstGeneration = graph.GetRecordingGeneration();
    EXPECT_NE(graphIdentity, 0u);
    EXPECT_NE(firstGeneration, 0u);

    const RHITextureDesc textureDesc =
        RHITextureDesc::RenderTarget(32, 32, RHIFormat::RGBA16_FLOAT);
    RHIBufferDesc bufferDesc;
    bufferDesc.size = 64;
    bufferDesc.usage = RHIBufferUsage::ShaderResource;

    const RGTextureHandle oldTexture = graph.CreateTexture(textureDesc);
    const RGBufferHandle oldBuffer = graph.CreateBuffer(bufferDesc);
    const RGTextureHandle oldSubresource = oldTexture.Subresource(0);
    const RGBufferHandle oldRange = oldBuffer.Range(8, 16);
    EXPECT_EQ(oldSubresource.graphIdentity, oldTexture.graphIdentity);
    EXPECT_EQ(oldSubresource.recordingGeneration, oldTexture.recordingGeneration);
    EXPECT_EQ(oldRange.graphIdentity, oldBuffer.graphIdentity);
    EXPECT_EQ(oldRange.recordingGeneration, oldBuffer.recordingGeneration);

    graph.Clear();
    EXPECT_EQ(graph.GetGraphIdentity(), graphIdentity);
    EXPECT_NE(graph.GetRecordingGeneration(), firstGeneration);
    EXPECT_NE(graph.GetRecordingGeneration(), 0u);

    const RGTextureHandle currentTexture = graph.CreateTexture(textureDesc);
    const RGBufferHandle currentBuffer = graph.CreateBuffer(bufferDesc);
    EXPECT_EQ(currentTexture.index, oldTexture.index);
    EXPECT_EQ(currentBuffer.index, oldBuffer.index);
    EXPECT_NE(currentTexture.recordingGeneration, oldTexture.recordingGeneration);
    EXPECT_NE(currentBuffer.recordingGeneration, oldBuffer.recordingGeneration);
    EXPECT_EQ(graph.GetTextureDesc(oldTexture), nullptr);
    EXPECT_EQ(graph.GetBufferDesc(oldBuffer), nullptr);
    EXPECT_NE(graph.GetTextureDesc(currentTexture), nullptr);
    EXPECT_NE(graph.GetBufferDesc(currentBuffer), nullptr);
    bool staleTextureRejected = false;
    bool staleBufferRejected = false;
    bool currentTextureAccepted = false;
    bool currentBufferAccepted = false;

    struct StaleHandlePassData
    {
        RGTextureHandle staleTexture;
        RGBufferHandle staleBuffer;
        RGTextureHandle currentTexture;
        RGBufferHandle currentBuffer;
    };
    graph.AddPass<StaleHandlePassData>(
        "RejectStaleGeneration",
        RenderGraphPassType::Graphics,
        [oldTexture,
         oldBuffer,
         currentTexture,
         currentBuffer,
         &staleTextureRejected,
         &staleBufferRejected,
         &currentTextureAccepted,
         &currentBufferAccepted](
            RenderGraphBuilder& builder,
            StaleHandlePassData& data)
        {
            data.staleTexture = builder.Read(oldTexture);
            data.staleBuffer = builder.Write(
                oldBuffer, RHIResourceState::UnorderedAccess);
            data.currentTexture = builder.Write(
                currentTexture, RHIResourceState::RenderTarget);
            data.currentBuffer = builder.Write(
                currentBuffer, RHIResourceState::UnorderedAccess);
            staleTextureRejected = !data.staleTexture.IsValid();
            staleBufferRejected = !data.staleBuffer.IsValid();
            currentTextureAccepted = data.currentTexture.IsValid();
            currentBufferAccepted = data.currentBuffer.IsValid();
        },
        [](const StaleHandlePassData&, RHICommandContext&) {});

    EXPECT_TRUE(staleTextureRejected);
    EXPECT_TRUE(staleBufferRejected);
    EXPECT_TRUE(currentTextureAccepted);
    EXPECT_TRUE(currentBufferAccepted);

    graph.SetExportState(currentTexture, RHIResourceState::Present);
    graph.Compile();
    EXPECT_FALSE(graph.GetCompileStats().compileValid);
    EXPECT_EQ(graph.GetCompileStats().invalidResourceUsageCount, 2u);
    ASSERT_FALSE(graph.GetCompileDiagnostics().empty());
    EXPECT_NE(graph.GetCompileDiagnostics().front().find("invalid"),
              std::string::npos);
}

TEST(RenderGraphValidation, InvalidBufferUsageIsReported)
{
    RenderGraph graph;
    RGBufferHandle invalidBuffer;
    invalidBuffer.index = 888;
    invalidBuffer.graphIdentity = graph.GetGraphIdentity();
    invalidBuffer.recordingGeneration = graph.GetRecordingGeneration();

    struct InvalidBufferPassData
    {
        RGBufferHandle buffer;
    };

    graph.AddPass<InvalidBufferPassData>(
        "InvalidBufferUsage",
        RenderGraphPassType::Compute,
        [invalidBuffer](RenderGraphBuilder& builder, InvalidBufferPassData& data)
        {
            data.buffer = builder.Write(invalidBuffer, RHIResourceState::UnorderedAccess);
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
    RGTextureHandle invalidTexture;
    invalidTexture.index = 999;
    invalidTexture.graphIdentity = graph.GetGraphIdentity();
    invalidTexture.recordingGeneration = graph.GetRecordingGeneration();

    graph.AddPass<SimplePassData>(
        "ComputeRenderTargetState",
        RenderGraphPassType::Compute,
        [invalidTexture](RenderGraphBuilder& builder, SimplePassData& data)
        {
            data.colorTarget = builder.Write(invalidTexture, RHIResourceState::RenderTarget);
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
