#include "Core/Log.h"
#include "Render/GPUDriven/GPUCulling.h"
#include "Render/Renderer/RenderDrawItem.h"
#include "Render/Renderer/RenderScene.h"
#include "RHI/RHI.h"
#include "RHI/RHICommandContext.h"
#include "ShaderCompiler/ShaderCompiler.h"

#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <vector>

using namespace RVX;

namespace
{
    class FakeBuffer final : public RHIBuffer
    {
    public:
        explicit FakeBuffer(const RHIBufferDesc& desc)
            : m_desc(desc)
            , m_storage(static_cast<size_t>(desc.size))
        {
            m_debugName = desc.debugName ? desc.debugName : "";
        }

        uint64 GetSize() const override { return m_desc.size; }
        RHIBufferUsage GetUsage() const override { return m_desc.usage; }
        RHIMemoryType GetMemoryType() const override { return m_desc.memoryType; }
        uint32 GetStride() const override { return m_desc.stride; }

        void* Map() override { return m_storage.empty() ? nullptr : m_storage.data(); }
        void Unmap() override {}

        const std::vector<uint8>& GetStorage() const { return m_storage; }
        void CopyFrom(const FakeBuffer& source, uint64 sourceOffset, uint64 destinationOffset, uint64 size)
        {
            if (sourceOffset + size > source.m_storage.size() ||
                destinationOffset + size > m_storage.size())
            {
                return;
            }

            std::memcpy(m_storage.data() + destinationOffset,
                        source.m_storage.data() + sourceOffset,
                        static_cast<size_t>(size));
        }

    private:
        RHIBufferDesc m_desc;
        std::vector<uint8> m_storage;
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
        void DrawIndexedIndirect(RHIBuffer* buffer, uint64 offset, uint32 drawCount, uint32 stride) override
        {
            ++drawIndexedIndirectCalls;
            lastIndirectBuffer = buffer;
            lastIndirectOffset = offset;
            lastIndirectDrawCount = drawCount;
            lastIndirectStride = stride;
        }
        void Dispatch(uint32, uint32, uint32) override {}
        void DispatchIndirect(RHIBuffer*, uint64) override {}
        void CopyBuffer(RHIBuffer* source,
                        RHIBuffer* destination,
                        uint64 sourceOffset,
                        uint64 destinationOffset,
                        uint64 size) override
        {
            auto* sourceBuffer = dynamic_cast<FakeBuffer*>(source);
            auto* destinationBuffer = dynamic_cast<FakeBuffer*>(destination);
            if (!sourceBuffer || !destinationBuffer)
            {
                return;
            }

            destinationBuffer->CopyFrom(*sourceBuffer, sourceOffset, destinationOffset, size);
        }
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

        uint32 drawIndexedIndirectCalls = 0;
        RHIBuffer* lastIndirectBuffer = nullptr;
        uint64 lastIndirectOffset = 0;
        uint32 lastIndirectDrawCount = 0;
        uint32 lastIndirectStride = 0;
    };

    class FakeDevice final : public IRHIDevice
    {
    public:
        RHIBufferRef CreateBuffer(const RHIBufferDesc& desc) override
        {
            RHIBufferRef buffer(new FakeBuffer(desc));
            createdBuffers.push_back(static_cast<FakeBuffer*>(buffer.Get()));
            return buffer;
        }

        RHITextureRef CreateTexture(const RHITextureDesc&) override { return {}; }
        RHITextureViewRef CreateTextureView(RHITexture*, const RHITextureViewDesc& = {}) override { return {}; }
        RHISamplerRef CreateSampler(const RHISamplerDesc&) override { return {}; }
        RHIShaderRef CreateShader(const RHIShaderDesc&) override { return {}; }
        RHIHeapRef CreateHeap(const RHIHeapDesc&) override { return {}; }
        RHITextureRef CreatePlacedTexture(RHIHeap*, uint64, const RHITextureDesc&) override { return {}; }
        RHIBufferRef CreatePlacedBuffer(RHIHeap*, uint64, const RHIBufferDesc&) override { return {}; }
        MemoryRequirements GetTextureMemoryRequirements(const RHITextureDesc&) override { return {}; }
        MemoryRequirements GetBufferMemoryRequirements(const RHIBufferDesc& desc) override { return {desc.size, 256}; }
        RHIDescriptorSetLayoutRef CreateDescriptorSetLayout(const RHIDescriptorSetLayoutDesc&) override { return {}; }
        RHIPipelineLayoutRef CreatePipelineLayout(const RHIPipelineLayoutDesc&) override { return {}; }
        RHIPipelineRef CreateGraphicsPipeline(const RHIGraphicsPipelineDesc&) override { return {}; }
        RHIPipelineRef CreateComputePipeline(const RHIComputePipelineDesc&) override { return {}; }
        RHIDescriptorSetRef CreateDescriptorSet(const RHIDescriptorSetDesc&) override { return {}; }
        RHIQueryPoolRef CreateQueryPool(const RHIQueryPoolDesc&) override { return {}; }
        RHICommandContextRef CreateCommandContext(RHICommandQueueType) override { return RHICommandContextRef(new FakeCommandContext()); }
        uint64 SubmitCommandContext(RHICommandContext*, RHIFence* = nullptr) override { return 0; }
        uint64 SubmitCommandContexts(std::span<RHICommandContext* const>, RHIFence* = nullptr) override { return 0; }
        RHISwapChainRef CreateSwapChain(const RHISwapChainDesc&) override { return {}; }
        RHIFenceRef CreateFence(uint64 = 0) override { return {}; }
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
        const RHICapabilities& GetCapabilities() const override { return capabilities; }
        RHIBackendType GetBackendType() const override { return RHIBackendType::DX12; }

        FakeBuffer* FindBuffer(const char* name) const
        {
            for (FakeBuffer* buffer : createdBuffers)
            {
                if (buffer && buffer->GetDebugName() == name)
                    return buffer;
            }
            return nullptr;
        }

        RHICapabilities capabilities;
        std::vector<FakeBuffer*> createdBuffers;
    };

    template <typename T>
    T ReadBufferValue(const FakeBuffer& buffer, size_t index = 0)
    {
        T value{};
        const size_t offset = index * sizeof(T);
        EXPECT_LE(offset + sizeof(T), buffer.GetStorage().size());
        if (offset + sizeof(T) <= buffer.GetStorage().size())
        {
            std::memcpy(&value, buffer.GetStorage().data() + offset, sizeof(T));
        }
        return value;
    }

    Mat4 TestView()
    {
        return lookAt(Vec3(0.0f, 0.0f, 0.0f),
                      Vec3(0.0f, 0.0f, -1.0f),
                      Vec3(0.0f, 1.0f, 0.0f));
    }

    Mat4 TestProjection()
    {
        return perspective(radians(60.0f), 1.0f, 0.1f, 200.0f);
    }

    GPUInstanceData MakeInstance(const Vec3& center, float radius, uint32 indexCount)
    {
        GPUInstanceData instance = {};
        instance.worldMatrix = Mat4(1.0f);
        instance.boundingSphere = Vec4(center, radius);
        instance.aabbMin = Vec4(center - Vec3(radius), 0.0f);
        instance.aabbMax = Vec4(center + Vec3(radius), 0.0f);
        instance.indexCount = indexCount;
        instance.firstIndex = 7;
        instance.vertexOffset = -2;
        return instance;
    }

    RenderObject MakeRenderObject(const Vec3& center, float extent, uint64 meshId)
    {
        RenderObject object;
        object.worldMatrix = Mat4(1.0f);
        object.worldMatrix[3] = Vec4(center, 1.0f);
        object.bounds = AABB(center - Vec3(extent), center + Vec3(extent));
        object.mesh = {static_cast<uint32>(meshId), 1};
        object.material = {static_cast<uint32>(meshId + 1000u), 1};
        object.drawable = true;
        object.visible = true;
        return object;
    }

    RenderDrawItem MakeDrawItem(uint32 objectIndex, uint64 meshId, uint64 materialId)
    {
        RenderDrawItem item;
        item.objectIndex = objectIndex;
        item.submeshIndex = 0;
        item.mesh = {static_cast<uint32>(meshId), 1};
        item.material = {static_cast<uint32>(materialId), 1};
        item.renderMode = MaterialRenderMode::Opaque;
        return item;
    }

    std::filesystem::path FindWorkspaceRoot()
    {
        std::filesystem::path path = std::filesystem::current_path();
        for (int i = 0; i < 8; ++i)
        {
            if (std::filesystem::exists(path / "Render" / "Private" / "Renderer" / "SceneRenderer.cpp"))
            {
                return path;
            }

            if (!path.has_parent_path())
            {
                break;
            }
            path = path.parent_path();
        }

        return {};
    }

    std::string ReadTextFile(const std::filesystem::path& path)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file)
        {
            return {};
        }

        return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    }
} // namespace

class GPUDrivenValidationFixture : public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        Log::Initialize();
    }

    static void TearDownTestSuite()
    {
        Log::Shutdown();
    }
};

TEST_F(GPUDrivenValidationFixture, CpuFallbackCullsInstancesAndBuildsIndirectCommands)
{
    FakeDevice device;

    GPUCullingConfig config;
    config.maxInstances = 8;
    config.enableDistanceCulling = true;
    config.maxDrawDistance = 20.0f;

    GPUCulling culling;
    culling.Initialize(&device, config);
    ASSERT_TRUE(culling.IsInitialized());

    culling.BeginFrame();
    EXPECT_EQ(0u, culling.AddInstance(MakeInstance(Vec3(0.0f, 0.0f, -5.0f), 1.0f, 36)));
    EXPECT_EQ(1u, culling.AddInstance(MakeInstance(Vec3(100.0f, 0.0f, -5.0f), 1.0f, 12)));
    EXPECT_EQ(2u, culling.AddInstance(MakeInstance(Vec3(0.0f, 0.0f, -100.0f), 1.0f, 24)));
    EXPECT_EQ(3u, culling.AddInstance(MakeInstance(Vec3(0.0f, 0.0f, -6.0f), 1.0f, 0)));
    culling.EndFrame();

    FakeCommandContext ctx;
    culling.Cull(ctx, TestView(), TestProjection());

    EXPECT_TRUE(culling.WasCpuFallbackUsedLastCull());
    EXPECT_EQ(1u, culling.GetDrawCount());
    ASSERT_EQ(1u, culling.GetVisibleInstanceIndices().size());
    EXPECT_EQ(0u, culling.GetVisibleInstanceIndices()[0]);

    const GPUCulling::Statistics stats = culling.GetStatistics();
    EXPECT_EQ(4u, stats.totalInstances);
    EXPECT_EQ(1u, stats.visibleInstances);
    EXPECT_EQ(1u, stats.frustumCulled);
    EXPECT_EQ(1u, stats.distanceCulled);
    EXPECT_EQ(0u, stats.occlusionCulled);

    ASSERT_EQ(1u, culling.GetIndirectCommands().size());
    const IndirectDrawIndexedCommand& command = culling.GetIndirectCommands()[0];
    EXPECT_EQ(36u, command.indexCount);
    EXPECT_EQ(1u, command.instanceCount);
    EXPECT_EQ(7u, command.firstIndex);
    EXPECT_EQ(-2, command.vertexOffset);
    EXPECT_EQ(0u, command.firstInstance);

    const FakeBuffer* drawCountBuffer = device.FindBuffer("GPUCulling.DrawCountBuffer");
    ASSERT_NE(nullptr, drawCountBuffer);
    EXPECT_EQ(1u, ReadBufferValue<uint32>(*drawCountBuffer));

    const FakeBuffer* visibleBuffer = device.FindBuffer("GPUCulling.VisibleInstanceBuffer");
    ASSERT_NE(nullptr, visibleBuffer);
    EXPECT_EQ(0u, ReadBufferValue<uint32>(*visibleBuffer));

    const FakeBuffer* indirectBuffer = device.FindBuffer("GPUCulling.IndirectDrawBuffer");
    ASSERT_NE(nullptr, indirectBuffer);
    const IndirectDrawIndexedCommand uploadedCommand =
        ReadBufferValue<IndirectDrawIndexedCommand>(*indirectBuffer);
    EXPECT_EQ(command.indexCount, uploadedCommand.indexCount);
    EXPECT_EQ(command.firstInstance, uploadedCommand.firstInstance);

    EXPECT_EQ(1u, culling.DrawIndexedIndirect(ctx));
    EXPECT_EQ(1u, ctx.drawIndexedIndirectCalls);
    EXPECT_EQ(culling.GetIndirectBuffer(), ctx.lastIndirectBuffer);
    EXPECT_EQ(0u, ctx.lastIndirectOffset);
    EXPECT_EQ(1u, ctx.lastIndirectDrawCount);
    EXPECT_EQ(sizeof(IndirectDrawIndexedCommand), ctx.lastIndirectStride);
}

TEST_F(GPUDrivenValidationFixture, CpuFallbackBuffersAvoidDx11InvalidGpuOnlyFlags)
{
    FakeDevice device;

    GPUCullingConfig config;
    config.maxInstances = 8;

    GPUCulling culling;
    culling.Initialize(&device, config);

    const FakeBuffer* visibilityBuffer = device.FindBuffer("GPUCulling.VisibilityBuffer");
    const FakeBuffer* visibleInstanceBuffer = device.FindBuffer("GPUCulling.VisibleInstanceBuffer");
    const FakeBuffer* indirectBuffer = device.FindBuffer("GPUCulling.IndirectDrawBuffer");
    const FakeBuffer* drawCountBuffer = device.FindBuffer("GPUCulling.DrawCountBuffer");
    ASSERT_NE(nullptr, visibilityBuffer);
    ASSERT_NE(nullptr, visibleInstanceBuffer);
    ASSERT_NE(nullptr, indirectBuffer);
    ASSERT_NE(nullptr, drawCountBuffer);

    const auto expectNoGpuOnlyFlags = [](const FakeBuffer& buffer) {
        EXPECT_FALSE(HasFlag(buffer.GetUsage(), RHIBufferUsage::Structured));
        EXPECT_FALSE(HasFlag(buffer.GetUsage(), RHIBufferUsage::ShaderResource));
        EXPECT_FALSE(HasFlag(buffer.GetUsage(), RHIBufferUsage::UnorderedAccess));
    };

    EXPECT_EQ(RHIMemoryType::Upload, visibilityBuffer->GetMemoryType());
    expectNoGpuOnlyFlags(*visibilityBuffer);

    EXPECT_EQ(RHIMemoryType::Upload, visibleInstanceBuffer->GetMemoryType());
    expectNoGpuOnlyFlags(*visibleInstanceBuffer);

    EXPECT_EQ(RHIMemoryType::Upload, drawCountBuffer->GetMemoryType());
    expectNoGpuOnlyFlags(*drawCountBuffer);
    EXPECT_FALSE(HasFlag(drawCountBuffer->GetUsage(), RHIBufferUsage::IndirectArgs));

    EXPECT_EQ(RHIMemoryType::Default, indirectBuffer->GetMemoryType());
    EXPECT_TRUE(HasFlag(indirectBuffer->GetUsage(), RHIBufferUsage::IndirectArgs));
    EXPECT_TRUE(HasFlag(indirectBuffer->GetUsage(), RHIBufferUsage::CopyDst));
    expectNoGpuOnlyFlags(*indirectBuffer);
}

TEST_F(GPUDrivenValidationFixture, GpuExecutionBuffersKeepStructuredUavFlags)
{
    FakeDevice device;
    device.capabilities.supportsComputePipeline = true;
    device.capabilities.supportsDescriptorSets = true;
    device.capabilities.supportsIndirectDrawCount = true;

    GPUCullingConfig config;
    config.maxInstances = 8;

    GPUCulling culling;
    culling.Initialize(&device, config);

    const FakeBuffer* visibilityBuffer = device.FindBuffer("GPUCulling.VisibilityBuffer");
    const FakeBuffer* indirectBuffer = device.FindBuffer("GPUCulling.IndirectDrawBuffer");
    const FakeBuffer* drawCountBuffer = device.FindBuffer("GPUCulling.DrawCountBuffer");
    ASSERT_NE(nullptr, visibilityBuffer);
    ASSERT_NE(nullptr, indirectBuffer);
    ASSERT_NE(nullptr, drawCountBuffer);

    const auto expectGpuWritableStructuredBuffer = [](const FakeBuffer& buffer) {
        EXPECT_EQ(RHIMemoryType::Default, buffer.GetMemoryType());
        EXPECT_TRUE(HasFlag(buffer.GetUsage(), RHIBufferUsage::Structured));
        EXPECT_TRUE(HasFlag(buffer.GetUsage(), RHIBufferUsage::ShaderResource));
        EXPECT_TRUE(HasFlag(buffer.GetUsage(), RHIBufferUsage::UnorderedAccess));
        EXPECT_TRUE(HasFlag(buffer.GetUsage(), RHIBufferUsage::CopyDst));
    };

    expectGpuWritableStructuredBuffer(*visibilityBuffer);
    expectGpuWritableStructuredBuffer(*indirectBuffer);
    expectGpuWritableStructuredBuffer(*drawCountBuffer);
    EXPECT_TRUE(HasFlag(indirectBuffer->GetUsage(), RHIBufferUsage::IndirectArgs));
    EXPECT_TRUE(HasFlag(drawCountBuffer->GetUsage(), RHIBufferUsage::IndirectArgs));
}

TEST_F(GPUDrivenValidationFixture, GpuExecutionDecisionReportsCapabilityAndPipelineFallbacks)
{
    {
        FakeDevice device;
        device.capabilities.supportsDescriptorSets = true;
        device.capabilities.supportsIndirectDrawCount = true;

        GPUCullingConfig config;
        config.maxInstances = 8;

        GPUCulling culling;
        culling.Initialize(&device, config);

        GPUCullingExecutionDecision decision = culling.GetExecutionDecision();
        EXPECT_EQ(GPUCullingExecutionMode::CpuFallback, decision.mode);
        EXPECT_FALSE(decision.gpuCapable);
        EXPECT_FALSE(decision.pipelineReady);
        EXPECT_EQ(GPUCullingFallbackReason::ComputePipelineUnsupported, decision.fallbackReason);
    }

    {
        FakeDevice device;
        device.capabilities.supportsComputePipeline = true;
        device.capabilities.supportsDescriptorSets = true;
        device.capabilities.supportsIndirectDrawCount = true;

        GPUCullingConfig config;
        config.maxInstances = 8;

        GPUCulling culling;
        culling.Initialize(&device, config);

        GPUCullingExecutionDecision decision = culling.GetExecutionDecision();
        EXPECT_EQ(GPUCullingExecutionMode::CpuFallback, decision.mode);
        EXPECT_TRUE(decision.gpuCapable);
        EXPECT_FALSE(decision.pipelineReady);
        EXPECT_EQ(GPUCullingFallbackReason::DescriptorSetLayoutCreationFailed, decision.fallbackReason);

        culling.BeginFrame();
        EXPECT_EQ(0u, culling.AddInstance(MakeInstance(Vec3(0.0f, 0.0f, -5.0f), 1.0f, 36)));
        culling.EndFrame();

        FakeCommandContext ctx;
        culling.Cull(ctx, TestView(), TestProjection());

        EXPECT_TRUE(culling.WasCpuFallbackUsedLastCull());
        EXPECT_FALSE(culling.WasGpuExecutionUsedLastCull());
        EXPECT_EQ(GPUCullingFallbackReason::DescriptorSetLayoutCreationFailed,
                  culling.GetLastFallbackReason());
    }
}

TEST_F(GPUDrivenValidationFixture, DrawIndexedIndirectHonorsMaxDrawCount)
{
    FakeDevice device;
    GPUCulling culling;
    culling.Initialize(&device, GPUCullingConfig{});

    culling.BeginFrame();
    EXPECT_EQ(0u, culling.AddInstance(MakeInstance(Vec3(0.0f, 0.0f, -5.0f), 1.0f, 12)));
    EXPECT_EQ(1u, culling.AddInstance(MakeInstance(Vec3(1.0f, 0.0f, -5.0f), 1.0f, 24)));
    culling.EndFrame();

    FakeCommandContext ctx;
    culling.Cull(ctx, TestView(), TestProjection());
    ASSERT_EQ(2u, culling.GetDrawCount());

    EXPECT_EQ(1u, culling.DrawIndexedIndirect(ctx, 1));
    EXPECT_EQ(1u, ctx.drawIndexedIndirectCalls);
    EXPECT_EQ(1u, ctx.lastIndirectDrawCount);
    EXPECT_EQ(sizeof(IndirectDrawIndexedCommand), ctx.lastIndirectStride);
}

TEST_F(GPUDrivenValidationFixture, CpuFallbackBuildsMeshGroupedIndirectRanges)
{
    FakeDevice device;

    GPUCullingConfig config;
    config.maxInstances = 8;
    config.enableDistanceCulling = false;

    GPUCulling culling;
    culling.Initialize(&device, config);

    culling.BeginFrame();
    ASSERT_EQ(0u, culling.BeginDrawGroup(7001u));
    EXPECT_EQ(0u, culling.AddInstance(MakeInstance(Vec3(0.0f, 0.0f, -5.0f), 1.0f, 36)));
    EXPECT_EQ(1u, culling.AddInstance(MakeInstance(Vec3(100.0f, 0.0f, -5.0f), 1.0f, 12)));
    culling.EndDrawGroup();
    ASSERT_EQ(1u, culling.BeginDrawGroup(7002u));
    EXPECT_EQ(2u, culling.AddInstance(MakeInstance(Vec3(0.0f, 0.0f, -6.0f), 1.0f, 24)));
    culling.EndDrawGroup();
    culling.EndFrame();

    FakeCommandContext ctx;
    culling.Cull(ctx, TestView(), TestProjection());

    ASSERT_EQ(2u, culling.GetDrawCount());
    ASSERT_EQ(2u, culling.GetDrawGroups().size());
    EXPECT_EQ(7001u, culling.GetDrawGroups()[0].meshId);
    EXPECT_EQ(0u, culling.GetDrawGroups()[0].commandOffset);
    EXPECT_EQ(sizeof(uint32), culling.GetDrawGroups()[0].countBufferOffset);
    EXPECT_EQ(2u, culling.GetDrawGroups()[0].maxDrawCount);
    EXPECT_EQ(1u, culling.GetDrawGroups()[0].visibleDrawCount);
    EXPECT_EQ(7002u, culling.GetDrawGroups()[1].meshId);
    EXPECT_EQ(2u, culling.GetDrawGroups()[1].commandOffset);
    EXPECT_EQ(sizeof(uint32) * 2u, culling.GetDrawGroups()[1].countBufferOffset);
    EXPECT_EQ(1u, culling.GetDrawGroups()[1].maxDrawCount);
    EXPECT_EQ(1u, culling.GetDrawGroups()[1].visibleDrawCount);

    const FakeBuffer* drawCountBuffer = device.FindBuffer("GPUCulling.DrawCountBuffer");
    ASSERT_NE(nullptr, drawCountBuffer);
    EXPECT_EQ(2u, ReadBufferValue<uint32>(*drawCountBuffer, 0));
    EXPECT_EQ(1u, ReadBufferValue<uint32>(*drawCountBuffer, 1));
    EXPECT_EQ(1u, ReadBufferValue<uint32>(*drawCountBuffer, 2));

    const FakeBuffer* indirectBuffer = device.FindBuffer("GPUCulling.IndirectDrawBuffer");
    ASSERT_NE(nullptr, indirectBuffer);
    const IndirectDrawIndexedCommand firstGroupCommand =
        ReadBufferValue<IndirectDrawIndexedCommand>(*indirectBuffer, 0);
    const IndirectDrawIndexedCommand secondGroupCommand =
        ReadBufferValue<IndirectDrawIndexedCommand>(*indirectBuffer, 2);
    EXPECT_EQ(36u, firstGroupCommand.indexCount);
    EXPECT_EQ(0u, firstGroupCommand.firstInstance);
    EXPECT_EQ(24u, secondGroupCommand.indexCount);
    EXPECT_EQ(2u, secondGroupCommand.firstInstance);

    EXPECT_EQ(0u, culling.DrawIndexedIndirect(ctx));
    EXPECT_EQ(0u, ctx.drawIndexedIndirectCalls);

    EXPECT_EQ(1u, culling.DrawIndexedIndirectGroup(ctx, 0));
    EXPECT_EQ(0u, ctx.lastIndirectOffset);
    EXPECT_EQ(1u, ctx.lastIndirectDrawCount);
    EXPECT_EQ(1u, culling.DrawIndexedIndirectGroup(ctx, 1));
    EXPECT_EQ(sizeof(IndirectDrawIndexedCommand) * 2u, ctx.lastIndirectOffset);
    EXPECT_EQ(1u, ctx.lastIndirectDrawCount);
}

TEST_F(GPUDrivenValidationFixture, DrawItemsMapBackThroughVisibleSourceIndices)
{
    FakeDevice device;

    GPUCullingConfig config;
    config.maxInstances = 8;
    config.enableDistanceCulling = true;
    config.maxDrawDistance = 30.0f;

    GPUCulling culling;
    culling.Initialize(&device, config);

    RenderScene scene;
    scene.AddObject(MakeRenderObject(Vec3(0.0f, 0.0f, -5.0f), 1.0f, 7001u));
    scene.AddObject(MakeRenderObject(Vec3(75.0f, 0.0f, -5.0f), 1.0f, 7002u));
    scene.AddObject(MakeRenderObject(Vec3(0.0f, 0.0f, -100.0f), 1.0f, 7003u));

    const std::vector<RenderDrawItem> drawItems = {
        MakeDrawItem(0, 7001u, 9001u),
        MakeDrawItem(1, 7002u, 9002u),
        MakeDrawItem(2, 7003u, 9003u),
    };

    culling.BeginFrame();
    EXPECT_EQ(0u, culling.AddDrawItemInstance(scene, drawItems[0], GPUIndexedDrawDesc{48u, 4u, -3}, 0u));
    EXPECT_EQ(1u, culling.AddDrawItemInstance(scene, drawItems[1], GPUIndexedDrawDesc{12u, 9u, 2}, 1u));
    EXPECT_EQ(2u, culling.AddDrawItemInstance(scene, drawItems[2], GPUIndexedDrawDesc{24u, 0u, 0}, 2u));
    culling.EndFrame();

    FakeCommandContext ctx;
    culling.Cull(ctx, TestView(), TestProjection());

    ASSERT_EQ(1u, culling.GetVisibleSourceIndices().size());
    EXPECT_EQ(0u, culling.GetVisibleSourceIndices()[0]);

    ASSERT_EQ(1u, culling.GetIndirectCommands().size());
    const IndirectDrawIndexedCommand& command = culling.GetIndirectCommands()[0];
    EXPECT_EQ(48u, command.indexCount);
    EXPECT_EQ(4u, command.firstIndex);
    EXPECT_EQ(-3, command.vertexOffset);
    EXPECT_EQ(0u, command.firstInstance);

    const GPUCulling::Statistics stats = culling.GetStatistics();
    EXPECT_EQ(3u, stats.totalInstances);
    EXPECT_EQ(1u, stats.visibleInstances);
    EXPECT_EQ(1u, stats.frustumCulled);
    EXPECT_EQ(1u, stats.distanceCulled);
}

TEST_F(GPUDrivenValidationFixture, SceneRendererWiresGpuCullingBeforePassResourceBinding)
{
    const std::filesystem::path root = FindWorkspaceRoot();
    ASSERT_FALSE(root.empty());

    const std::string header =
        ReadTextFile(root / "Render" / "Include" / "Render" / "Renderer" / "SceneRenderer.h");
    const std::string source =
        ReadTextFile(root / "Render" / "Private" / "Renderer" / "SceneRenderer.cpp");
    const std::string depthHeader =
        ReadTextFile(root / "Render" / "Include" / "Render" / "Passes" / "DepthPrepass.h");
    const std::string depthSource =
        ReadTextFile(root / "Render" / "Private" / "Passes" / "DepthPrepass.cpp");
    ASSERT_FALSE(header.empty());
    ASSERT_FALSE(source.empty());
    ASSERT_FALSE(depthHeader.empty());
    ASSERT_FALSE(depthSource.empty());

    EXPECT_NE(header.find("SceneGPUDrivenCullingStats"), std::string::npos);
    EXPECT_NE(header.find("graphPassAdded"), std::string::npos);
    EXPECT_NE(header.find("graphPassRecorded"), std::string::npos);
    EXPECT_NE(header.find("gpuExecutionRecorded"), std::string::npos);
    EXPECT_NE(header.find("graphInputDrawItemCount"), std::string::npos);
    EXPECT_NE(header.find("opaqueIndirectRequested"), std::string::npos);
    EXPECT_NE(header.find("opaqueGpuDrivenIndirectDrawCount"), std::string::npos);
    EXPECT_NE(header.find("SetGPUDrivenCullingEnabled"), std::string::npos);
    EXPECT_NE(header.find("const SceneGPUDrivenCullingStats& GetGPUDrivenCullingStats() const"),
              std::string::npos);
    EXPECT_NE(header.find("void AddGPUDrivenCullingPass()"), std::string::npos);
    EXPECT_NE(header.find("void PrepareGPUDrivenGraphCullInputs()"), std::string::npos);

    const size_t buildDrawLists = source.find("void SceneRenderer::BuildMaterialDrawLists()");
    ASSERT_NE(buildDrawLists, std::string::npos);
    const size_t cullingCall = source.find("ApplyGPUDrivenCullingToDrawLists();", buildDrawLists);
    const size_t objectVelocityBind = source.find("m_objectVelocityPass->SetRenderScene", buildDrawLists);
    ASSERT_NE(cullingCall, std::string::npos);
    ASSERT_NE(objectVelocityBind, std::string::npos);
    EXPECT_LT(cullingCall, objectVelocityBind);

    EXPECT_NE(source.find("m_gpuCulling->CullCpuFallback"), std::string::npos);
    EXPECT_EQ(source.find("drawItems.swap(m_gpuCullingScratchDrawItems);"), std::string::npos);
    EXPECT_NE(source.find("PrepareGPUDrivenGraphCullInputs();"), std::string::npos);
    EXPECT_NE(source.find("queueDrawItems(m_opaqueDrawItems);"), std::string::npos);
    EXPECT_NE(source.find("queueDrawItems(m_maskedDrawItems);"), std::string::npos);
    EXPECT_NE(source.find("m_gpuCulling->BeginDrawGroup("), std::string::npos);
    EXPECT_NE(source.find("group.materialId"), std::string::npos);
    EXPECT_NE(source.find("group.pipelineVariant"), std::string::npos);
    EXPECT_NE(source.find("group.material = item.material"), std::string::npos);

    const size_t buildGraph = source.find("void SceneRenderer::BuildRenderGraph()");
    ASSERT_NE(buildGraph, std::string::npos);
    const size_t cullGraphCall = source.find("AddGPUDrivenCullingPass();", buildGraph);
    const size_t passRegistryLoop = source.find("for (auto& pass : m_passRegistry->GetPasses())", buildGraph);
    ASSERT_NE(cullGraphCall, std::string::npos);
    ASSERT_NE(passRegistryLoop, std::string::npos);
    EXPECT_LT(cullGraphCall, passRegistryLoop);

    EXPECT_NE(source.find("\"GPUDrivenCull\""), std::string::npos);
    EXPECT_NE(source.find("RenderGraphPassType::Compute"), std::string::npos);
    EXPECT_NE(source.find("builder.Read(data.constants, RHIResourceState::ConstantBuffer"), std::string::npos);
    EXPECT_NE(source.find("builder.Read(data.instances, RHIShaderStage::Compute)"), std::string::npos);
    EXPECT_NE(source.find("builder.Write(data.indirectDraws, RHIResourceState::UnorderedAccess)"),
              std::string::npos);
    EXPECT_NE(source.find("m_depthPrepass->SetGPUDrivenRenderGraphResources"), std::string::npos);
    EXPECT_NE(source.find("m_opaquePass->SetGPUDrivenRenderGraphResources"), std::string::npos);
    EXPECT_NE(source.find("m_gpuCulling->Cull(ctx, m_viewData.viewMatrix, m_viewData.projectionMatrix)"),
              std::string::npos);
    EXPECT_NE(source.find("m_gpuDrivenCullingStats.gpuExecutionRecorded"), std::string::npos);
    EXPECT_NE(source.find("m_opaquePass->GetDrawStats()"), std::string::npos);
    EXPECT_NE(source.find("m_gpuDrivenCullingStats.opaqueIndirectRequested"), std::string::npos);
    EXPECT_NE(source.find("m_gpuDrivenCullingStats.opaqueGpuDrivenIndirectDrawCount"), std::string::npos);
    EXPECT_NE(depthHeader.find("SetGPUDrivenRenderGraphResources"), std::string::npos);
    EXPECT_NE(depthSource.find("builder.Read(m_gpuDrivenInstanceHandle, RHIShaderStage::Vertex)"),
              std::string::npos);
    EXPECT_NE(depthSource.find("builder.Read(m_gpuDrivenIndirectHandle, RHIResourceState::IndirectArgument)"),
              std::string::npos);
    EXPECT_NE(depthSource.find("builder.Read(m_gpuDrivenDrawCountHandle, RHIResourceState::IndirectArgument)"),
              std::string::npos);
}

TEST_F(GPUDrivenValidationFixture, SceneRendererFrameDiagnosticsExposeGPUDrivenExecutionDecision)
{
    const std::filesystem::path root = FindWorkspaceRoot();
    ASSERT_FALSE(root.empty());

    const std::string header =
        ReadTextFile(root / "Render" / "Include" / "Render" / "Renderer" / "SceneRenderer.h");
    const std::string source =
        ReadTextFile(root / "Render" / "Private" / "Renderer" / "SceneRenderer.cpp");
    ASSERT_FALSE(header.empty());
    ASSERT_FALSE(source.empty());

    EXPECT_NE(header.find("bool executionDecisionAvailable = false;"), std::string::npos);
    EXPECT_NE(header.find("GPUCullingExecutionDecision executionDecision;"), std::string::npos);
    EXPECT_NE(header.find("SceneGPUDrivenCullingStats gpuDrivenCullingStats;"), std::string::npos);
    EXPECT_NE(source.find("diagnostics.gpuDrivenCullingStats = m_gpuDrivenCullingStats;"),
              std::string::npos);
    EXPECT_NE(source.find("m_gpuDrivenCullingStats.executionDecisionAvailable = true;"),
              std::string::npos);
    EXPECT_NE(source.find("m_gpuDrivenCullingStats.executionDecision = m_gpuCulling->GetExecutionDecision();"),
              std::string::npos);
}

TEST_F(GPUDrivenValidationFixture, GPUCullingDeclaresComputeCompactionAndIndirectCountContracts)
{
    const std::filesystem::path root = FindWorkspaceRoot();
    ASSERT_FALSE(root.empty());

    const std::string header =
        ReadTextFile(root / "Render" / "Include" / "Render" / "GPUDriven" / "GPUCulling.h");
    const std::string source =
        ReadTextFile(root / "Render" / "Private" / "GPUDriven" / "GPUCulling.cpp");
    const std::string rhiCommandContext =
        ReadTextFile(root / "RHI" / "Include" / "RHI" / "RHICommandContext.h");
    const std::string shader =
        ReadTextFile(root / "Render" / "Shaders" / "GPUDriven" / "GPUCulling.hlsl");
    ASSERT_FALSE(header.empty());
    ASSERT_FALSE(source.empty());
    ASSERT_FALSE(rhiCommandContext.empty());
    ASSERT_FALSE(shader.empty());

    EXPECT_NE(header.find("RHIDescriptorSetRef m_cullingDescriptorSet"), std::string::npos);
    EXPECT_NE(header.find("RHIPipelineRef m_frustumCullPipeline"), std::string::npos);
    EXPECT_NE(header.find("RHIPipelineRef m_compactPipeline"), std::string::npos);
    EXPECT_NE(header.find("WasGpuExecutionUsedLastCull"), std::string::npos);
    EXPECT_NE(header.find("GPUCullingFallbackReason"), std::string::npos);
    EXPECT_NE(header.find("GetExecutionDecision"), std::string::npos);
    EXPECT_NE(header.find("GetLastFallbackReason"), std::string::npos);

    EXPECT_NE(source.find("CreatePipelineResources()"), std::string::npos);
    EXPECT_NE(source.find("EvaluateGpuExecution"), std::string::npos);
    EXPECT_NE(source.find("supportsComputePipeline"), std::string::npos);
    EXPECT_NE(source.find("CreateComputePipeline"), std::string::npos);
    EXPECT_NE(source.find("ctx.SetDescriptorSet(0, m_cullingDescriptorSet.Get())"), std::string::npos);
    EXPECT_NE(source.find("ctx.DrawIndexedIndirectCount"), std::string::npos);

    EXPECT_NE(rhiCommandContext.find("DrawIndexedIndirectCount"), std::string::npos);
    EXPECT_NE(shader.find("void CSFrustumCull"), std::string::npos);
    EXPECT_NE(shader.find("void CSCompactDraws"), std::string::npos);
    EXPECT_NE(shader.find("gDrawCount[instanceIndex] = 0"), std::string::npos);
    EXPECT_NE(shader.find("InterlockedAdd(gDrawCount[0], 1, totalDrawIndex)"), std::string::npos);
    EXPECT_NE(shader.find("InterlockedAdd(gDrawCount[instance.drawGroupIndex + 1], 1, groupDrawIndex)"),
              std::string::npos);
    EXPECT_NE(shader.find("instance.drawGroupCommandOffset + groupDrawIndex"), std::string::npos);
    EXPECT_NE(shader.find("command.firstInstance = instanceIndex"), std::string::npos);
}

TEST_F(GPUDrivenValidationFixture, OpaquePassDeclaresGPUDrivenDefaultLitIndirectContracts)
{
    const std::filesystem::path root = FindWorkspaceRoot();
    ASSERT_FALSE(root.empty());

    const std::string defaultLit =
        ReadTextFile(root / "Render" / "Shaders" / "DefaultLit.hlsl");
    const std::string pipelineHeader =
        ReadTextFile(root / "Render" / "Include" / "Render" / "PipelineCache.h");
    const std::string pipelineSource =
        ReadTextFile(root / "Render" / "Private" / "PipelineCache.cpp");
    const std::string opaqueHeader =
        ReadTextFile(root / "Render" / "Include" / "Render" / "Passes" / "OpaquePass.h");
    const std::string opaqueSource =
        ReadTextFile(root / "Render" / "Private" / "Passes" / "OpaquePass.cpp");
    const std::string modelViewer =
        ReadTextFile(root / "Samples" / "Showcase" / "ModelViewer" / "main.cpp");
    const std::string testsCMake = ReadTextFile(root / "Tests" / "CMakeLists.txt");
    ASSERT_FALSE(defaultLit.empty());
    ASSERT_FALSE(pipelineHeader.empty());
    ASSERT_FALSE(pipelineSource.empty());
    ASSERT_FALSE(opaqueHeader.empty());
    ASSERT_FALSE(opaqueSource.empty());
    ASSERT_FALSE(modelViewer.empty());
    ASSERT_FALSE(testsCMake.empty());

    const size_t ensureIndirectCapacity = opaqueSource.find("bool OpaquePass::EnsureIndirectDrawCapacity");
    ASSERT_NE(ensureIndirectCapacity, std::string::npos);
    const size_t setupFunction = opaqueSource.find("void OpaquePass::Setup", ensureIndirectCapacity);
    ASSERT_NE(setupFunction, std::string::npos);
    const std::string ensureIndirectBody =
        opaqueSource.substr(ensureIndirectCapacity, setupFunction - ensureIndirectCapacity);
    EXPECT_NE(ensureIndirectBody.find("RHIBufferUsage::IndirectArgs | RHIBufferUsage::CopyDst"),
              std::string::npos);
    EXPECT_EQ(ensureIndirectBody.find("RHIBufferUsage::Structured"), std::string::npos);

    EXPECT_NE(defaultLit.find("StructuredBuffer<GPUInstanceData> GPUDrivenInstances"), std::string::npos);
    EXPECT_NE(defaultLit.find("PSInput VSMainGPUDriven"), std::string::npos);
    EXPECT_NE(defaultLit.find("instance.normalMatrix"), std::string::npos);
    EXPECT_NE(pipelineHeader.find("GetGPUDrivenPipelineForVariant"), std::string::npos);
    EXPECT_NE(pipelineSource.find("GPUDrivenOpaquePipeline"), std::string::npos);
    EXPECT_NE(pipelineSource.find("RVX_PIPELINE_PURPOSE_GPU_DRIVEN_DEFAULT"), std::string::npos);
    EXPECT_NE(opaqueHeader.find("SetGPUDrivenCullingSource"), std::string::npos);
    EXPECT_NE(opaqueHeader.find("SetGPUDrivenRenderGraphResources"), std::string::npos);
    EXPECT_NE(opaqueSource.find("TryDrawGPUDrivenIndirect"), std::string::npos);
    EXPECT_NE(opaqueSource.find("DrawIndexedIndirectGroup"), std::string::npos);
    EXPECT_NE(modelViewer.find("--expect-gpu-driven-culling-ready"), std::string::npos);
    EXPECT_NE(modelViewer.find("--gpu-driven-culling-test-scene"), std::string::npos);
    EXPECT_NE(modelViewer.find("--disable-gpu-driven-culling"), std::string::npos);
    EXPECT_NE(modelViewer.find(
                  "frameSettings.gpuCulling.enabled = !options.disableGPUDrivenCulling"),
              std::string::npos);
    EXPECT_NE(modelViewer.find("IsGPUDrivenCullingReady"), std::string::npos);
    EXPECT_NE(modelViewer.find("opaqueGpuDrivenIndirectDrawCount"), std::string::npos);
    EXPECT_NE(modelViewer.find("stats.graphInputDrawItemCount > stats.visibleCullableDrawItemCount"),
              std::string::npos);
    EXPECT_NE(testsCMake.find("ModelViewerGPUDrivenSmoke"), std::string::npos);
    EXPECT_NE(testsCMake.find("ModelViewerGPUDrivenDisabledSmoke"), std::string::npos);
    EXPECT_NE(testsCMake.find("GPUDrivenVisualGoldenValidation"), std::string::npos);
    EXPECT_NE(testsCMake.find("GPUDrivenDisabledVisualDiffValidation"), std::string::npos);
    EXPECT_NE(testsCMake.find("R11_GPUDriven_DX12_320x180.ppm"), std::string::npos);
    EXPECT_NE(testsCMake.find("--gpu-driven-culling-test-scene"), std::string::npos);
    EXPECT_NE(testsCMake.find("--disable-gpu-driven-culling"), std::string::npos);
    EXPECT_NE(testsCMake.find("--min-different-pixels 1"), std::string::npos);
}

TEST_F(GPUDrivenValidationFixture, GPUCullingComputeShaderEntriesCompileForDX12)
{
    const std::filesystem::path root = FindWorkspaceRoot();
    ASSERT_FALSE(root.empty());

    const std::filesystem::path shaderPath =
        root / "Render" / "Shaders" / "GPUDriven" / "GPUCulling.hlsl";
    const std::string shader = ReadTextFile(shaderPath);
    ASSERT_FALSE(shader.empty());

    std::unique_ptr<IShaderCompiler> compiler = CreateShaderCompiler();
    ASSERT_NE(nullptr, compiler);

    const std::string shaderPathString = shaderPath.string();
    ShaderCompileOptions options;
    options.stage = RHIShaderStage::Compute;
    options.sourceCode = shader.c_str();
    options.sourcePath = shaderPathString.c_str();
    options.targetBackend = RHIBackendType::DX12;
    options.targetProfile = "cs_6_0";
    options.enableDebugInfo = false;
    options.enableOptimization = true;

    const ShaderCompileSupport support = compiler->QuerySupport(options);
    if (!support.IsSupported())
    {
        GTEST_SKIP() << support.reason;
    }

    options.entryPoint = "CSFrustumCull";
    ShaderCompileResult frustumResult = compiler->Compile(options);
    ASSERT_TRUE(frustumResult.success) << frustumResult.errorMessage;
    EXPECT_FALSE(frustumResult.bytecode.empty());

    options.entryPoint = "CSCompactDraws";
    ShaderCompileResult compactResult = compiler->Compile(options);
    ASSERT_TRUE(compactResult.success) << compactResult.errorMessage;
    EXPECT_FALSE(compactResult.bytecode.empty());
}
