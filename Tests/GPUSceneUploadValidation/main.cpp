#include "GPUScene/GPUSceneUploader.h"
#include "Render/Graph/RenderGraph.h"
#include "Resources/RenderRetirementQueue.h"
#include "Resources/RenderSubmissionResourceBatch.h"
#include "Resources/RenderSubmissionTracker.h"
#include "RHI/RHICommandContext.h"
#include "RHI/RHIDevice.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <vector>

using namespace RVX;

namespace
{
    class FakeBuffer final : public RHIBuffer
    {
    public:
        explicit FakeBuffer(const RHIBufferDesc& desc)
            : m_desc(desc), m_bytes(static_cast<size_t>(desc.size))
        {
            SetDebugName(desc.debugName);
        }

        uint64 GetSize() const override { return m_desc.size; }
        RHIBufferUsage GetUsage() const override { return m_desc.usage; }
        RHIMemoryType GetMemoryType() const override { return m_desc.memoryType; }
        uint32 GetStride() const override { return m_desc.stride; }
        void* Map() override
        {
            ++m_mapCalls;
            return m_failMap || m_bytes.empty() ? nullptr : m_bytes.data();
        }
        void Unmap() override {}
        void SetMapFailure(bool enabled) { m_failMap = enabled; }

        void CopyFrom(const FakeBuffer& source, uint64 sourceOffset,
                      uint64 targetOffset, uint64 size)
        {
            if (sourceOffset + size > source.m_bytes.size() ||
                targetOffset + size > m_bytes.size())
            {
                return;
            }
            std::memcpy(m_bytes.data() + targetOffset,
                        source.m_bytes.data() + sourceOffset,
                        static_cast<size_t>(size));
        }

        const std::vector<uint8>& Bytes() const { return m_bytes; }
        uint32 MapCalls() const { return m_mapCalls; }

    private:
        RHIBufferDesc m_desc;
        std::vector<uint8> m_bytes;
        uint32 m_mapCalls = 0;
        bool m_failMap = false;
    };

    class FakeFence final : public RHIFence
    {
    public:
        explicit FakeFence(uint64 value) : m_completed(value), m_next(value + 1) {}
        uint64 GetCompletedValue() const override { return m_completed; }
        void Signal(uint64 value) override { Complete(value); }
        void SignalOnQueue(uint64 value, RHICommandQueueType) override { Complete(value); }
        void Wait(uint64 value, uint64 = UINT64_MAX) override { Complete(value); }
        uint64 Allocate() { return m_next++; }
        void Complete(uint64 value)
        {
            m_completed = std::max(m_completed, value);
            m_next = std::max(m_next, value + 1U);
        }
    private:
        uint64 m_completed = 0;
        uint64 m_next = 1;
    };

    class FakeCommandContext final : public RHICommandContext
    {
    public:
        explicit FakeCommandContext(
            RHICommandQueueType queue = RHICommandQueueType::Graphics)
            : m_queue(queue)
        {
        }

        RHICommandQueueType GetQueueType() const override { return m_queue; }
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
        void DrawIndexedIndirectCount(RHIBuffer*, uint64, RHIBuffer*, uint64, uint32, uint32) override {}
        void Dispatch(uint32, uint32, uint32) override {}
        void DispatchIndirect(RHIBuffer*, uint64) override {}
        void CopyBuffer(RHIBuffer* source, RHIBuffer* target,
                        uint64 sourceOffset, uint64 targetOffset, uint64 size) override
        {
            ++copyCount;
            auto* sourceBuffer = dynamic_cast<FakeBuffer*>(source);
            auto* targetBuffer = dynamic_cast<FakeBuffer*>(target);
            if (sourceBuffer && targetBuffer)
            {
                targetBuffer->CopyFrom(*sourceBuffer, sourceOffset, targetOffset, size);
            }
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
        void SetDepthBias(float, float, float = 0.0F) override {}
        void SetDepthBounds(float, float) override {}
        void SetStencilReferenceSeparate(uint32, uint32) override {}
        void SetLineWidth(float) override {}
        void SignalFence(RHIFence*, uint64) override {}
        void WaitFence(RHIFence*, uint64) override {}

        uint32 copyCount = 0;
    private:
        RHICommandQueueType m_queue = RHICommandQueueType::Graphics;
    };

    class FakeDevice final : public IRHIDevice
    {
    public:
        explicit FakeDevice(bool compatibility = false)
        {
            m_compatibility = compatibility;
            m_capabilities.backendType = compatibility
                ? RHIBackendType::DX11
                : RHIBackendType::DX12;
            m_capabilities.adapterName = "GPUSceneUploadFake";
            m_capabilities.driverVersion = "1";
            m_capabilities.supportsComputePipeline = true;
            m_capabilities.supportsDescriptorSets = true;
            m_capabilities.supportsDynamicDescriptorOffsets = true;
            m_capabilities.maxDescriptorSets = 4;
            m_capabilities.supportsExplicitResourceBarriers = true;
            m_capabilities.supportsDefaultQueueFenceSignal = !compatibility;
            m_capabilities.supportsExplicitQueueFenceSignal = !compatibility;
            m_capabilities.emulatesQueueFences = compatibility;
            m_capabilities.supportsAsyncCompute = !compatibility;
            m_capabilities.dx12.resourceBindingTier = 2;
            m_capabilities.queueTopology.completionMode = compatibility
                ? RHIQueueCompletionMode::CompatibilityWaitIdle
                : RHIQueueCompletionMode::NativeTimeline;
            m_capabilities.queueTopology.logicalQueueDomains = compatibility
                ? std::array<GPUQueueDomain, 3>{GPUQueueDomain::Graphics,
                                                  GPUQueueDomain::Graphics,
                                                  GPUQueueDomain::Graphics}
                : std::array<GPUQueueDomain, 3>{GPUQueueDomain::Graphics,
                                                  GPUQueueDomain::Compute,
                                                  GPUQueueDomain::Copy};
            m_capabilities.queueTopology.activeDomainCount = compatibility ? 1 : 3;
        }

        RHIBufferRef CreateBuffer(const RHIBufferDesc& desc) override
        {
            if (m_createFailureCountdown == 0)
            {
                m_createFailureCountdown = -1;
                return {};
            }
            if (m_createFailureCountdown > 0)
            {
                --m_createFailureCountdown;
            }
            RHIBufferRef result(new FakeBuffer(desc));
            if (m_failUploadMap && desc.memoryType == RHIMemoryType::Upload)
            {
                static_cast<FakeBuffer*>(result.Get())->SetMapFailure(true);
            }
            m_buffers.push_back(static_cast<FakeBuffer*>(result.Get()));
            return result;
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
        RHICommandContextRef CreateCommandContext(RHICommandQueueType) override { return {}; }
        uint64 SubmitCommandContext(RHICommandContext*, RHIFence* fence = nullptr) override
        {
            return fence ? static_cast<FakeFence*>(fence)->Allocate() : 0;
        }
        uint64 SubmitCommandContexts(std::span<RHICommandContext* const>, RHIFence* = nullptr) override { return 0; }
        RHISwapChainRef CreateSwapChain(const RHISwapChainDesc&) override { return {}; }
        RHIFenceRef CreateFence(uint64 value = 0) override
        {
            RHIFenceRef result(new FakeFence(value));
            m_fences.push_back(result);
            return result;
        }
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
        RHIBackendType GetBackendType() const override
        {
            return m_compatibility ? RHIBackendType::DX11 : RHIBackendType::DX12;
        }
        RHIDeviceRuntimeStatus QueryRuntimeStatus() const noexcept override
        {
            return m_deviceLost ? RHIDeviceRuntimeStatus::DeviceLost
                                : RHIDeviceRuntimeStatus::Ready;
        }

        FakeBuffer* Find(const char* name) const
        {
            for (FakeBuffer* buffer : m_buffers)
            {
                if (buffer && buffer->GetDebugName() == name &&
                    buffer->GetMemoryType() == RHIMemoryType::Default)
                {
                    return buffer;
                }
            }
            return nullptr;
        }
        FakeFence* Fence(uint32 index) const
        {
            return index < m_fences.size() ? static_cast<FakeFence*>(m_fences[index].Get()) : nullptr;
        }
        uint32 DefaultBufferCount() const
        {
            uint32 count = 0;
            for (FakeBuffer* buffer : m_buffers)
            {
                count += buffer && buffer->GetMemoryType() == RHIMemoryType::Default;
            }
            return count;
        }
        void FailCreateAfter(int32 calls) { m_createFailureCountdown = calls; }
        void SetFailUploadMap(bool enabled) { m_failUploadMap = enabled; }
        void SetDeviceLost(bool lost) { m_deviceLost = lost; }

    private:
        RHICapabilities m_capabilities;
        std::vector<FakeBuffer*> m_buffers;
        std::vector<RHIFenceRef> m_fences;
        int32 m_createFailureCountdown = -1;
        bool m_failUploadMap = false;
        bool m_deviceLost = false;
        bool m_compatibility = false;
    };

    GPUSceneObjectData MakeObject(uint64 id, float32 marker)
    {
        GPUSceneObjectData object;
        object.objectId = id;
        object.bounds.minimum = {marker, 0.0F, 0.0F, 0.0F};
        object.bounds.maximum = {marker + 1.0F, 1.0F, 1.0F, 0.0F};
        object.draws.resize(1);
        object.draws[0].draw.indexCount = 3;
        object.draws[0].draw.instanceCount = 1;
        object.draws[0].geometry.indexCount = 3;
        return object;
    }

    void RecordAndExecute(GPUSceneUploader& uploader, FakeDevice& device,
                           FakeCommandContext& context)
    {
        RenderGraph graph;
        graph.SetDevice(&device);
        uploader.BuildRenderGraph(graph, nullptr);
        graph.Compile();
        ASSERT_TRUE(graph.GetCompileStats().compileValid);
        graph.Execute(context);
        uploader.CommitRealizedAccess(graph);
    }

    struct GPUSceneLeaseReadPassData
    {
        std::array<RGBufferHandle, GPU_SCENE_RESIDENT_TABLE_COUNT> handles;
    };

    [[nodiscard]] bool RecordExactLeaseRead(
        GPUSceneUploader& uploader,
        FakeDevice& device,
        FakeCommandContext& context,
        uint64 requiredVersion)
    {
        RenderGraph graph;
        graph.SetDevice(&device);
        const std::optional<GPUSceneResidentGraphLease> lease =
            uploader.AcquireCurrentGraphLease(
                graph, nullptr, requiredVersion);
        if (!lease)
        {
            return false;
        }
        const auto addLeaseReadPass =
            [&graph, &lease](const char* name)
        {
        graph.AddPass<GPUSceneLeaseReadPassData>(
            name,
            RenderGraphPassType::Compute,
            [lease](RenderGraphBuilder& builder, GPUSceneLeaseReadPassData& data)
            {
                for (uint32 tableIndex = 0;
                     tableIndex < GPU_SCENE_RESIDENT_TABLE_COUNT;
                     ++tableIndex)
                {
                    data.handles[tableIndex] = builder.Read(
                        lease->handles[tableIndex],
                        MakeRHIAccessSnapshot(RHIResourceState::ShaderResource,
                                              RHIShaderStage::Compute));
                }
            },
            [](const GPUSceneLeaseReadPassData&, RHICommandContext&) {});
        };
        // One exact lease can feed independent Depth/Opaque-like consumers;
        // no second AcquireCurrentGraphLease call is permitted for this graph.
        addLeaseReadPass("GPUSceneLeaseReadA");
        addLeaseReadPass("GPUSceneLeaseReadB");
        graph.Compile();
        if (!graph.GetCompileStats().compileValid)
        {
            return false;
        }
        graph.Execute(context);
        uploader.CommitRealizedAccess(graph);
        return true;
    }

    GPUCompletionToken SubmitToken(
        RenderSubmissionTracker& tracker,
        std::span<FakeCommandContext* const> contexts)
    {
        GPUCompletionToken token;
        for (FakeCommandContext* context : contexts)
        {
            const GPUCompletionPoint point = tracker.Submit(context);
            EXPECT_NE(point.value, 0U);
            EXPECT_TRUE(InsertGPUCompletionPoint(token, point));
        }
        return token;
    }

    void CompleteToken(FakeDevice& device, const GPUCompletionToken& token)
    {
        for (uint8 index = 0; index < token.count; ++index)
        {
            const uint32 fenceIndex = static_cast<uint32>(token.points[index].domain);
            ASSERT_NE(device.Fence(fenceIndex), nullptr);
            device.Fence(fenceIndex)->Complete(token.points[index].value);
        }
    }

    TEST(GPUSceneUploadValidation, FullThenIncrementalUploadKeepsUntouchedRows)
    {
        FakeDevice device;
        RenderSubmissionTracker tracker;
        ASSERT_TRUE(tracker.Initialize(&device));
        GPUSceneUploader uploader;
        ASSERT_TRUE(uploader.Initialize(&device, &tracker));
        GPUSceneDatabase database;
        GPUSceneTransaction add;
        add.Add(MakeObject(1, 1.0F));
        ASSERT_TRUE(database.Commit(add).Succeeded());
        uploader.Observe(database.GetCommittedMirror(), database.GetLastChangeSet());

        FakeCommandContext context;
        RecordAndExecute(uploader, device, context);
        EXPECT_GT(context.copyCount, 0U);
        const GPUCompletionPoint point = tracker.Submit(&context);
        ASSERT_NE(point.value, 0U);
        GPUCompletionToken token;
        ASSERT_TRUE(InsertGPUCompletionPoint(token, point));
        uploader.NotifySubmission(token);
        ASSERT_NE(device.Fence(0), nullptr);
        device.Fence(0)->Complete(point.value);

        FakeBuffer* primitive = device.Find("GPUScene.Primitives");
        ASSERT_NE(primitive, nullptr);
        const std::vector<uint8> before = primitive->Bytes();
        FakeBuffer* bounds = device.Find("GPUScene.Bounds");
        ASSERT_NE(bounds, nullptr);
        const std::vector<uint8> boundsBefore = bounds->Bytes();

        const GPUScenePrimitiveRef primitiveRef = database.FindPrimitive(1).value();
        GPUSceneTransaction update;
        update.Update(primitiveRef, MakeObject(1, 7.0F));
        ASSERT_TRUE(database.Commit(update).Succeeded());
        uploader.Observe(database.GetCommittedMirror(), database.GetLastChangeSet());
        context.copyCount = 0;
        RecordAndExecute(uploader, device, context);
        EXPECT_GT(context.copyCount, 0U);
        EXPECT_LT(uploader.GetDiagnostics().frameUploadBytes,
                  uploader.GetDiagnostics().persistentBytes);
        // Only the bounds row changed; an unchanged table is not rewritten.
        EXPECT_EQ(before, primitive->Bytes());
        EXPECT_NE(boundsBefore, bounds->Bytes());
    }

    TEST(GPUSceneUploadValidation,
         OnePercentDirtyObjectsGenerateExactPerTableUploadRanges)
    {
        FakeDevice device;
        RenderSubmissionTracker tracker;
        ASSERT_TRUE(tracker.Initialize(&device));
        GPUSceneUploader uploader;
        ASSERT_TRUE(uploader.Initialize(&device, &tracker));
        GPUSceneDatabase database;

        constexpr uint32 objectCount = 1'000;
        constexpr uint32 dirtyCount = objectCount / 100U;
        GPUSceneTransaction add;
        for (uint32 index = 0; index < objectCount; ++index)
        {
            add.Add(MakeObject(
                static_cast<uint64>(index) + 1U,
                static_cast<float32>(index)));
        }
        ASSERT_TRUE(database.Commit(add).Succeeded());
        uploader.Observe(database.GetCommittedMirror(),
                         database.GetLastChangeSet());

        FakeCommandContext context;
        RecordAndExecute(uploader, device, context);
        const GPUCompletionPoint fullUpload = tracker.Submit(&context);
        ASSERT_NE(fullUpload.value, 0U);
        GPUCompletionToken token;
        ASSERT_TRUE(InsertGPUCompletionPoint(token, fullUpload));
        uploader.NotifySubmission(token);
        ASSERT_NE(device.Fence(0), nullptr);
        device.Fence(0)->Complete(fullUpload.value);

        GPUSceneTransaction update;
        for (uint32 index = 0; index < objectCount; index += 100U)
        {
            const uint64 objectId = static_cast<uint64>(index) + 1U;
            const std::optional<GPUScenePrimitiveRef> primitive =
                database.FindPrimitive(objectId);
            ASSERT_TRUE(primitive.has_value());
            update.Update(
                *primitive,
                MakeObject(objectId, static_cast<float32>(index) + 0.5F));
        }
        ASSERT_TRUE(database.Commit(update).Succeeded());
        uploader.Observe(database.GetCommittedMirror(),
                         database.GetLastChangeSet());
        context.copyCount = 0;
        RecordAndExecute(uploader, device, context);

        const GPUSceneUploadDiagnostics& diagnostics =
            uploader.GetDiagnostics();
        constexpr std::array<uint64, GPU_SCENE_RESIDENT_TABLE_COUNT>
            rowSizes = {
                sizeof(GPUScenePrimitiveRow),
                sizeof(GPUSceneBoundsRow),
                sizeof(GPUSceneTransformRow),
                sizeof(GPUSceneMaterialRow),
                sizeof(GPUSceneGeometryRow),
                sizeof(GPUSceneDrawMetadataRow),
            };
        uint64 expectedBytes = 0;
        for (uint32 tableIndex = 0;
             tableIndex < GPU_SCENE_RESIDENT_TABLE_COUNT;
             ++tableIndex)
        {
            EXPECT_EQ(diagnostics.tables[tableIndex].frameUploadRangeCount,
                      dirtyCount);
            EXPECT_EQ(diagnostics.tables[tableIndex].frameUploadBytes,
                      static_cast<uint64>(dirtyCount) * rowSizes[tableIndex]);
            expectedBytes +=
                static_cast<uint64>(dirtyCount) * rowSizes[tableIndex];
        }
        EXPECT_EQ(diagnostics.frameUploadRangeCount,
                  dirtyCount * GPU_SCENE_RESIDENT_TABLE_COUNT);
        EXPECT_EQ(diagnostics.frameUploadBytes, expectedBytes);
    }

    TEST(GPUSceneUploadValidation, WarmStaticFrameAndUnsubmittedRetryAreSafe)
    {
        FakeDevice device;
        RenderSubmissionTracker tracker;
        ASSERT_TRUE(tracker.Initialize(&device));
        GPUSceneUploader uploader;
        ASSERT_TRUE(uploader.Initialize(&device, &tracker));
        GPUSceneDatabase database;
        GPUSceneTransaction add;
        add.Add(MakeObject(1, 1.0F));
        ASSERT_TRUE(database.Commit(add).Succeeded());
        uploader.Observe(database.GetCommittedMirror(), database.GetLastChangeSet());

        FakeCommandContext context;
        RecordAndExecute(uploader, device, context);
        uploader.ReleaseUnsubmittedFrame();
        EXPECT_TRUE(uploader.GetDiagnostics().rollbackPending);
        RecordAndExecute(uploader, device, context);
        const GPUCompletionPoint point = tracker.Submit(&context);
        GPUCompletionToken token;
        ASSERT_TRUE(InsertGPUCompletionPoint(token, point));
        uploader.NotifySubmission(token);
        device.Fence(0)->Complete(point.value);

        RenderGraph graph;
        graph.SetDevice(&device);
        uploader.BuildRenderGraph(graph, nullptr);
        graph.Compile();
        EXPECT_TRUE(graph.GetCompileStats().compileValid);
        EXPECT_EQ(graph.GetCompileStats().totalPasses, 0U);
        EXPECT_EQ(uploader.GetDiagnostics().frameUploadBytes, 0U);
    }

    TEST(GPUSceneUploadValidation, InFlightStaticVersionStaysZeroCopyWithoutSafetyProbe)
    {
        FakeDevice device;
        RenderSubmissionTracker tracker;
        ASSERT_TRUE(tracker.Initialize(&device));
        GPUSceneUploader uploader;
        ASSERT_TRUE(uploader.Initialize(&device, &tracker));
        GPUSceneDatabase database;
        GPUSceneTransaction add;
        add.Add(MakeObject(1, 1.0F));
        ASSERT_TRUE(database.Commit(add).Succeeded());
        uploader.Observe(database.GetCommittedMirror(), database.GetLastChangeSet());
        FakeCommandContext context;
        RecordAndExecute(uploader, device, context);
        const GPUCompletionPoint point = tracker.Submit(&context);
        GPUCompletionToken token;
        ASSERT_TRUE(InsertGPUCompletionPoint(token, point));
        uploader.NotifySubmission(token);

        RenderGraph graph;
        graph.SetDevice(&device);
        uploader.BuildRenderGraph(graph, nullptr);
        graph.Compile();
        EXPECT_EQ(graph.GetCompileStats().totalPasses, 0U);
        EXPECT_EQ(uploader.GetDiagnostics().frameUploadBytes, 0U);
        EXPECT_EQ(uploader.GetDiagnostics().frameUploadRangeCount, 0U);
    }

    TEST(GPUSceneUploadValidation,
         ExactVersionReadinessIsSideEffectFreeAndAcquireRequiresTheFrozenVersion)
    {
        GPUSceneUploader uninitialized;
        const GPUSceneResidentReadiness unavailable =
            uninitialized.QueryExactVersionReadiness(17u);
        EXPECT_EQ(GPUSceneResidentReadinessStatus::Unavailable,
                  unavailable.status);
        EXPECT_EQ(17u, unavailable.requiredVersion);
        EXPECT_FALSE(unavailable.IsReady());

        FakeDevice device;
        RenderSubmissionTracker tracker;
        ASSERT_TRUE(tracker.Initialize(&device));
        GPUSceneUploader uploader;
        ASSERT_TRUE(uploader.Initialize(&device, &tracker));
        GPUSceneDatabase database;
        GPUSceneTransaction add;
        add.Add(MakeObject(1, 1.0F));
        ASSERT_TRUE(database.Commit(add).Succeeded());
        const uint64 version = database.GetCommittedVersion();
        uploader.Observe(database.GetCommittedMirror(), database.GetLastChangeSet());

        const GPUSceneUploadDiagnostics diagnosticsBefore =
            uploader.GetDiagnostics();
        const uint32 defaultBuffersBefore = device.DefaultBufferCount();
        const GPUSceneResidentReadiness pendingA =
            uploader.QueryExactVersionReadiness(version);
        const GPUSceneResidentReadiness pendingB =
            uploader.QueryExactVersionReadiness(version);
        EXPECT_EQ(GPUSceneResidentReadinessStatus::Pending, pendingA.status);
        EXPECT_EQ(pendingA, pendingB);
        EXPECT_EQ(version, pendingA.requiredVersion);
        EXPECT_EQ(version, pendingA.observedVersion);
        EXPECT_FALSE(pendingA.IsReady());
        EXPECT_EQ(defaultBuffersBefore, device.DefaultBufferCount());
        const GPUSceneUploadDiagnostics diagnosticsAfter =
            uploader.GetDiagnostics();
        EXPECT_EQ(diagnosticsBefore.observedVersion,
                  diagnosticsAfter.observedVersion);
        EXPECT_EQ(diagnosticsBefore.residentVersion,
                  diagnosticsAfter.residentVersion);
        EXPECT_EQ(diagnosticsBefore.frameUploadBytes,
                  diagnosticsAfter.frameUploadBytes);
        EXPECT_EQ(diagnosticsBefore.frameUploadRangeCount,
                  diagnosticsAfter.frameUploadRangeCount);
        EXPECT_EQ(diagnosticsBefore.failureReason,
                  diagnosticsAfter.failureReason);

        FakeCommandContext context;
        RecordAndExecute(uploader, device, context);
        const GPUCompletionPoint uploadPoint = tracker.Submit(&context);
        GPUCompletionToken uploadToken;
        ASSERT_TRUE(InsertGPUCompletionPoint(uploadToken, uploadPoint));
        uploader.NotifySubmission(uploadToken);
        CompleteToken(device, uploadToken);

        const GPUSceneResidentReadiness ready =
            uploader.QueryExactVersionReadiness(version);
        EXPECT_EQ(GPUSceneResidentReadinessStatus::Ready, ready.status);
        EXPECT_EQ(version, ready.requiredVersion);
        EXPECT_EQ(version, ready.observedVersion);
        EXPECT_EQ(version, ready.residentVersion);
        EXPECT_TRUE(ready.IsReady());
        const GPUSceneResidentReadiness wrongVersion =
            uploader.QueryExactVersionReadiness(version + 1u);
        EXPECT_EQ(GPUSceneResidentReadinessStatus::Unavailable,
                  wrongVersion.status);
        EXPECT_EQ(version + 1u, wrongVersion.requiredVersion);
        EXPECT_EQ(version, wrongVersion.observedVersion);
        EXPECT_FALSE(wrongVersion.IsReady());

        RenderGraph graph;
        graph.SetDevice(&device);
        EXPECT_FALSE(uploader.AcquireCurrentGraphLease(
            graph, nullptr, version + 1u).has_value());
        EXPECT_TRUE(uploader.AcquireCurrentGraphLease(
            graph, nullptr, version).has_value());
        EXPECT_EQ(GPUSceneResidentReadinessStatus::Pending,
                  uploader.QueryExactVersionReadiness(version).status);
        EXPECT_TRUE(uploader.CancelCurrentGraphLease());
        EXPECT_TRUE(uploader.QueryExactVersionReadiness(version).IsReady());

        GPUSceneTransaction update;
        update.Update(database.FindPrimitive(1).value(), MakeObject(1, 2.0F));
        ASSERT_TRUE(database.Commit(update).Succeeded());
        const uint64 nextVersion = database.GetCommittedVersion();
        uploader.Observe(database.GetCommittedMirror(), database.GetLastChangeSet());
        const GPUSceneResidentReadiness dirty =
            uploader.QueryExactVersionReadiness(nextVersion);
        EXPECT_EQ(GPUSceneResidentReadinessStatus::Pending, dirty.status);
        EXPECT_EQ(nextVersion, dirty.observedVersion);
        EXPECT_FALSE(dirty.IsReady());

        device.SetDeviceLost(true);
        const GPUSceneResidentReadiness lost =
            uploader.QueryExactVersionReadiness(nextVersion);
        EXPECT_EQ(GPUSceneResidentReadinessStatus::Unavailable, lost.status);
        EXPECT_FALSE(lost.IsReady());
    }

    TEST(GPUSceneUploadValidation, ExactCurrentLeaseRetainsOneSixTableSetAndRollsBackReadAccess)
    {
        FakeDevice device;
        RenderSubmissionTracker tracker;
        ASSERT_TRUE(tracker.Initialize(&device));
        RenderRetirementQueue retirement;
        ASSERT_TRUE(retirement.Initialize(&tracker));
        GPUSceneUploader uploader;
        ASSERT_TRUE(uploader.Initialize(&device, &tracker));
        GPUSceneDatabase database;
        GPUSceneTransaction add;
        add.Add(MakeObject(1, 1.0F));
        ASSERT_TRUE(database.Commit(add).Succeeded());
        uploader.Observe(database.GetCommittedMirror(), database.GetLastChangeSet());

        FakeCommandContext context;
        RecordAndExecute(uploader, device, context);
        const GPUCompletionPoint uploadPoint = tracker.Submit(&context);
        GPUCompletionToken uploadToken;
        ASSERT_TRUE(InsertGPUCompletionPoint(uploadToken, uploadPoint));
        uploader.NotifySubmission(uploadToken);
        CompleteToken(device, uploadToken);

        RenderGraph graph;
        graph.SetDevice(&device);
        RenderSubmissionResourceBatch batch;
        const std::optional<GPUSceneResidentGraphLease> lease =
            uploader.AcquireCurrentGraphLease(
                graph, &batch, database.GetCommittedVersion());
        ASSERT_TRUE(lease.has_value());
        ASSERT_TRUE(lease->IsValid());
        EXPECT_EQ(lease->version, database.GetCommittedVersion());
        EXPECT_EQ(batch.GetRetainedObjectCount(), GPU_SCENE_RESIDENT_TABLE_COUNT);
        for (uint32 tableIndex = 0;
             tableIndex < GPU_SCENE_RESIDENT_TABLE_COUNT;
             ++tableIndex)
        {
            EXPECT_NE(lease->buffers[tableIndex], nullptr);
            EXPECT_TRUE(lease->handles[tableIndex].IsValid());
            EXPECT_GT(lease->capacities[tableIndex], 0U);
        }
        // A recording owns exactly one concrete set; a second lease cannot
        // accidentally mark another same-version set as in flight.
        EXPECT_FALSE(uploader.AcquireCurrentGraphLease(
            graph, &batch, database.GetCommittedVersion()).has_value());

        struct LeaseReadPassData
        {
            std::array<RGBufferHandle, GPU_SCENE_RESIDENT_TABLE_COUNT> handles;
        };
        struct RasterLeaseReadPassData
        {
            RGBufferHandle candidate;
            RGBufferHandle primitive;
            RGBufferHandle transform;
        };
        const auto addLeaseReadPass =
            [&graph, &lease](const char* name)
        {
        graph.AddPass<LeaseReadPassData>(
            name,
            RenderGraphPassType::Compute,
            [lease](RenderGraphBuilder& builder, LeaseReadPassData& data)
            {
                for (uint32 tableIndex = 0;
                     tableIndex < GPU_SCENE_RESIDENT_TABLE_COUNT;
                     ++tableIndex)
                {
                    data.handles[tableIndex] = builder.Read(
                        lease->handles[tableIndex],
                        MakeRHIAccessSnapshot(RHIResourceState::ShaderResource,
                                              RHIShaderStage::Compute));
                }
            },
            [](const LeaseReadPassData&, RHICommandContext&) {});
        };
        addLeaseReadPass("GPUSceneLeaseReadA");
        addLeaseReadPass("GPUSceneLeaseReadB");

        RHIBufferDesc candidateDesc;
        candidateDesc.size = sizeof(uint32);
        candidateDesc.stride = sizeof(uint32);
        candidateDesc.usage = RHIBufferUsage::Structured | RHIBufferUsage::ShaderResource;
        candidateDesc.memoryType = RHIMemoryType::Default;
        candidateDesc.debugName = "GPUSceneLeaseRasterCandidate";
        const RHIBufferRef candidateBuffer = device.CreateBuffer(candidateDesc);
        ASSERT_NE(candidateBuffer, nullptr);
        const RGBufferHandle candidateHandle = graph.ImportBuffer(
            candidateBuffer.Get(),
            MakeRHIBufferAccessSnapshot(RHIResourceState::ShaderResource,
                                        RHIShaderStage::Vertex));
        ASSERT_TRUE(candidateHandle.IsValid());
        graph.AddPass<RasterLeaseReadPassData>(
            "GPUSceneLeaseRasterRead",
            RenderGraphPassType::Graphics,
            [candidateHandle, lease](RenderGraphBuilder& builder,
                                     RasterLeaseReadPassData& data)
            {
                data.candidate = builder.Read(
                    candidateHandle,
                    MakeRHIAccessSnapshot(RHIResourceState::ShaderResource,
                                          RHIShaderStage::Vertex));
                data.primitive = builder.Read(
                    lease->handles[static_cast<uint32>(GPUSceneResidentTable::Primitives)],
                    MakeRHIAccessSnapshot(RHIResourceState::ShaderResource,
                                          RHIShaderStage::Vertex));
                data.transform = builder.Read(
                    lease->handles[static_cast<uint32>(GPUSceneResidentTable::Transforms)],
                    MakeRHIAccessSnapshot(RHIResourceState::ShaderResource,
                                          RHIShaderStage::Vertex));
            },
            [](const RasterLeaseReadPassData&, RHICommandContext&) {});
        graph.Compile();
        ASSERT_TRUE(graph.GetCompileStats().compileValid);
        EXPECT_EQ(3u, graph.GetCompileStats().totalPasses);
        graph.Execute(context);
        uploader.CommitRealizedAccess(graph);
        EXPECT_FALSE(uploader.CancelCurrentGraphLease());
        for (uint32 tableIndex = 0;
             tableIndex < GPU_SCENE_RESIDENT_TABLE_COUNT;
             ++tableIndex)
        {
            EXPECT_EQ(ProjectRHIResourceState(
                          graph.GetRealizedAccess(lease->handles[tableIndex]).uniformAccess),
                      RHIResourceState::ShaderResource);
        }
        for (const RGBufferHandle handle : {
                 candidateHandle,
                 lease->handles[static_cast<uint32>(GPUSceneResidentTable::Primitives)],
                 lease->handles[static_cast<uint32>(GPUSceneResidentTable::Transforms)]})
        {
            const RHIBufferAccessSnapshot realized = graph.GetRealizedAccess(handle);
            EXPECT_EQ(ProjectRHIResourceState(realized.uniformAccess),
                      RHIResourceState::ShaderResource);
            EXPECT_NE(static_cast<uint32>(
                          realized.uniformAccess.executionScope &
                          RHIExecutionScope::VertexShader),
                      0U);
        }

        uploader.ReleaseUnsubmittedFrame();
        EXPECT_TRUE(uploader.GetDiagnostics().rollbackPending);
        RenderGraph retryGraph;
        retryGraph.SetDevice(&device);
        EXPECT_TRUE(uploader.AcquireCurrentGraphLease(
            retryGraph, nullptr, database.GetCommittedVersion()).has_value());
        uploader.ReleaseUnsubmittedFrame();
        batch.ReleaseUnsubmitted(retirement);
    }

    TEST(GPUSceneUploadValidation,
         ExactCurrentLeaseCanBeCancelledOnlyBeforeConsumerAccessCommits)
    {
        FakeDevice device;
        RenderSubmissionTracker tracker;
        ASSERT_TRUE(tracker.Initialize(&device));
        GPUSceneUploader uploader;
        ASSERT_TRUE(uploader.Initialize(&device, &tracker));
        EXPECT_FALSE(uploader.CancelCurrentGraphLease());
        GPUSceneDatabase database;
        GPUSceneTransaction add;
        add.Add(MakeObject(1, 1.0F));
        ASSERT_TRUE(database.Commit(add).Succeeded());
        uploader.Observe(database.GetCommittedMirror(), database.GetLastChangeSet());

        FakeCommandContext context;
        RecordAndExecute(uploader, device, context);
        const GPUCompletionPoint uploadPoint = tracker.Submit(&context);
        GPUCompletionToken uploadToken;
        ASSERT_TRUE(InsertGPUCompletionPoint(uploadToken, uploadPoint));
        uploader.NotifySubmission(uploadToken);
        CompleteToken(device, uploadToken);

        RenderGraph unusedGraph;
        unusedGraph.SetDevice(&device);
        ASSERT_TRUE(uploader.AcquireCurrentGraphLease(
            unusedGraph, nullptr, database.GetCommittedVersion()).has_value());
        EXPECT_TRUE(uploader.CancelCurrentGraphLease());
        EXPECT_FALSE(uploader.CancelCurrentGraphLease());

        RenderGraph retryGraph;
        retryGraph.SetDevice(&device);
        EXPECT_TRUE(uploader.AcquireCurrentGraphLease(
            retryGraph, nullptr, database.GetCommittedVersion()).has_value());
        uploader.ReleaseUnsubmittedFrame();
    }

    TEST(GPUSceneUploadValidation, ExactCurrentLeaseRejectsPendingAndObservedVersionMismatch)
    {
        FakeDevice device;
        RenderSubmissionTracker tracker;
        ASSERT_TRUE(tracker.Initialize(&device));
        GPUSceneUploader uploader;
        ASSERT_TRUE(uploader.Initialize(&device, &tracker));
        GPUSceneDatabase database;
        GPUSceneTransaction add;
        add.Add(MakeObject(1, 1.0F));
        ASSERT_TRUE(database.Commit(add).Succeeded());
        uploader.Observe(database.GetCommittedMirror(), database.GetLastChangeSet());

        RenderGraph pendingGraph;
        pendingGraph.SetDevice(&device);
        uploader.BuildRenderGraph(pendingGraph, nullptr);
        EXPECT_FALSE(uploader.AcquireCurrentGraphLease(
            pendingGraph, nullptr, database.GetCommittedVersion()).has_value());

        FakeCommandContext context;
        pendingGraph.Compile();
        ASSERT_TRUE(pendingGraph.GetCompileStats().compileValid);
        pendingGraph.Execute(context);
        uploader.CommitRealizedAccess(pendingGraph);
        const GPUCompletionPoint uploadPoint = tracker.Submit(&context);
        GPUCompletionToken uploadToken;
        ASSERT_TRUE(InsertGPUCompletionPoint(uploadToken, uploadPoint));
        uploader.NotifySubmission(uploadToken);

        RenderGraph residentGraph;
        residentGraph.SetDevice(&device);
        ASSERT_TRUE(uploader.AcquireCurrentGraphLease(
            residentGraph, nullptr, database.GetCommittedVersion()).has_value());
        uploader.ReleaseUnsubmittedFrame();

        GPUSceneTransaction update;
        update.Update(database.FindPrimitive(1).value(), MakeObject(1, 2.0F));
        ASSERT_TRUE(database.Commit(update).Succeeded());
        uploader.Observe(database.GetCommittedMirror(), database.GetLastChangeSet());
        RenderGraph mismatchGraph;
        mismatchGraph.SetDevice(&device);
        EXPECT_FALSE(uploader.AcquireCurrentGraphLease(
            mismatchGraph, nullptr, database.GetCommittedVersion()).has_value());
        uploader.BuildRenderGraph(mismatchGraph, nullptr);
        EXPECT_FALSE(uploader.AcquireCurrentGraphLease(
            mismatchGraph, nullptr, database.GetCommittedVersion()).has_value());
        uploader.ReleaseUnsubmittedFrame();
    }

    TEST(GPUSceneUploadValidation, CommittedExactLeaseUsesItsSubmissionUntilCompletion)
    {
        FakeDevice device;
        RenderSubmissionTracker tracker;
        ASSERT_TRUE(tracker.Initialize(&device));
        GPUSceneUploader uploader;
        ASSERT_TRUE(uploader.Initialize(&device, &tracker));
        GPUSceneDatabase database;
        GPUSceneTransaction add;
        add.Add(MakeObject(1, 1.0F));
        ASSERT_TRUE(database.Commit(add).Succeeded());
        uploader.Observe(database.GetCommittedMirror(), database.GetLastChangeSet());

        FakeCommandContext context;
        RecordAndExecute(uploader, device, context);
        const GPUCompletionPoint uploaded = tracker.Submit(&context);
        GPUCompletionToken uploadToken;
        ASSERT_TRUE(InsertGPUCompletionPoint(uploadToken, uploaded));
        uploader.NotifySubmission(uploadToken);
        CompleteToken(device, uploadToken);

        ASSERT_TRUE(RecordExactLeaseRead(
            uploader, device, context, database.GetCommittedVersion()));
        const GPUCompletionPoint readPoint = tracker.Submit(&context);
        GPUCompletionToken readToken;
        ASSERT_TRUE(InsertGPUCompletionPoint(readToken, readPoint));
        uploader.NotifySubmission(readToken);
        EXPECT_FALSE(uploader.CancelCurrentGraphLease());
        EXPECT_EQ(uploader.GetDiagnostics().failureReason,
                  GPUSceneUploadFailureReason::None);
        EXPECT_EQ(uploader.PollSafeReclaimVersion(), 0U);

        const uint32 defaultBufferCount = device.DefaultBufferCount();
        GPUSceneTransaction update;
        update.Update(database.FindPrimitive(1).value(), MakeObject(1, 2.0F));
        ASSERT_TRUE(database.Commit(update).Succeeded());
        uploader.Observe(database.GetCommittedMirror(), database.GetLastChangeSet());
        RenderGraph graph;
        graph.SetDevice(&device);
        uploader.BuildRenderGraph(graph, nullptr);
        // The submitted exact lease is still pending, so updating V2 must not
        // overwrite V1's set in place.
        EXPECT_GE(device.DefaultBufferCount(), defaultBufferCount + 6U);
        uploader.ReleaseUnsubmittedFrame();
    }

    TEST(GPUSceneUploadValidation, ExactLeaseTwoReadersRetainVersionUntilCompletionAndRejectedReadRollsBack)
    {
        FakeDevice device;
        RenderSubmissionTracker tracker;
        ASSERT_TRUE(tracker.Initialize(&device));
        GPUSceneUploader uploader;
        ASSERT_TRUE(uploader.Initialize(&device, &tracker));
        GPUSceneDatabase database;
        GPUSceneTransaction add;
        add.Add(MakeObject(1, 1.0F));
        ASSERT_TRUE(database.Commit(add).Succeeded());
        const uint64 versionOne = database.GetCommittedVersion();
        uploader.Observe(database.GetCommittedMirror(), database.GetLastChangeSet());

        FakeCommandContext context;
        RecordAndExecute(uploader, device, context);
        const GPUCompletionPoint uploaded = tracker.Submit(&context);
        GPUCompletionToken uploadToken;
        ASSERT_TRUE(InsertGPUCompletionPoint(uploadToken, uploaded));
        uploader.NotifySubmission(uploadToken);
        CompleteToken(device, uploadToken);
        ASSERT_TRUE(uploader.QueryExactVersionReadiness(versionOne).IsReady());

        // A graph rejected before execution releases its one outstanding exact
        // lease and restores V1's readable state for a later accepted frame.
        RenderGraph rejectedGraph;
        rejectedGraph.SetDevice(&device);
        ASSERT_TRUE(uploader.AcquireCurrentGraphLease(
            rejectedGraph, nullptr, versionOne).has_value());
        uploader.ReleaseUnsubmittedFrame();
        ASSERT_TRUE(uploader.QueryExactVersionReadiness(versionOne).IsReady());

        // RecordExactLeaseRead adds two independent consumers to one lease,
        // matching the Depth/Opaque multi-reader ownership contract.
        ASSERT_TRUE(RecordExactLeaseRead(uploader, device, context, versionOne));
        const GPUCompletionPoint readPoint = tracker.Submit(&context);
        GPUCompletionToken readToken;
        ASSERT_TRUE(InsertGPUCompletionPoint(readToken, readPoint));
        uploader.NotifySubmission(readToken);

        const uint32 buffersBeforeV2 = device.DefaultBufferCount();
        GPUSceneTransaction update;
        update.Update(database.FindPrimitive(1).value(), MakeObject(1, 2.0F));
        ASSERT_TRUE(database.Commit(update).Succeeded());
        uploader.Observe(database.GetCommittedMirror(), database.GetLastChangeSet());
        RenderGraph versionTwoGraph;
        versionTwoGraph.SetDevice(&device);
        uploader.BuildRenderGraph(versionTwoGraph, nullptr);
        // V1 is still in flight, so V2 cannot overwrite the exact multi-reader
        // set. The uploader allocates a distinct six-table resident set.
        EXPECT_GE(device.DefaultBufferCount(), buffersBeforeV2 + 6U);
        uploader.ReleaseUnsubmittedFrame();
        EXPECT_EQ(uploader.PollSafeReclaimVersion(), 0U);

        CompleteToken(device, readToken);
        EXPECT_GE(uploader.PollSafeReclaimVersion(), versionOne);
    }

    TEST(GPUSceneUploadValidation, ExactLeaseCommitAndNotifyFailuresFailClosed)
    {
        FakeDevice device;
        RenderSubmissionTracker tracker;
        ASSERT_TRUE(tracker.Initialize(&device));
        GPUSceneUploader uploader;
        ASSERT_TRUE(uploader.Initialize(&device, &tracker));
        GPUSceneDatabase database;
        GPUSceneTransaction add;
        add.Add(MakeObject(1, 1.0F));
        ASSERT_TRUE(database.Commit(add).Succeeded());
        uploader.Observe(database.GetCommittedMirror(), database.GetLastChangeSet());

        FakeCommandContext context;
        RecordAndExecute(uploader, device, context);
        const GPUCompletionPoint uploaded = tracker.Submit(&context);
        GPUCompletionToken uploadToken;
        ASSERT_TRUE(InsertGPUCompletionPoint(uploadToken, uploaded));
        uploader.NotifySubmission(uploadToken);
        CompleteToken(device, uploadToken);

        ASSERT_TRUE(RecordExactLeaseRead(
            uploader, device, context, database.GetCommittedVersion()));
        GPUCompletionToken invalid;
        invalid.count = 1;
        invalid.points[0] = {GPUQueueDomain::Graphics, 0};
        uploader.NotifySubmission(invalid);
        EXPECT_EQ(uploader.GetDiagnostics().failureReason,
                  GPUSceneUploadFailureReason::InvalidCompletionToken);
        RenderGraph afterInvalid;
        afterInvalid.SetDevice(&device);
        EXPECT_FALSE(uploader.AcquireCurrentGraphLease(
            afterInvalid, nullptr, database.GetCommittedVersion()).has_value());

        GPUSceneUploader omittedCommitUploader;
        ASSERT_TRUE(omittedCommitUploader.Initialize(&device, &tracker));
        GPUSceneDatabase omittedCommitDatabase;
        GPUSceneTransaction omittedAdd;
        omittedAdd.Add(MakeObject(2, 2.0F));
        ASSERT_TRUE(omittedCommitDatabase.Commit(omittedAdd).Succeeded());
        omittedCommitUploader.Observe(
            omittedCommitDatabase.GetCommittedMirror(),
            omittedCommitDatabase.GetLastChangeSet());
        RecordAndExecute(omittedCommitUploader, device, context);
        const GPUCompletionPoint omittedUploaded = tracker.Submit(&context);
        GPUCompletionToken omittedUploadToken;
        ASSERT_TRUE(InsertGPUCompletionPoint(omittedUploadToken, omittedUploaded));
        omittedCommitUploader.NotifySubmission(omittedUploadToken);
        CompleteToken(device, omittedUploadToken);

        RenderGraph omittedGraph;
        omittedGraph.SetDevice(&device);
        ASSERT_TRUE(omittedCommitUploader.AcquireCurrentGraphLease(
            omittedGraph, nullptr,
            omittedCommitDatabase.GetCommittedVersion()).has_value());
        const GPUCompletionPoint omittedRead = tracker.Submit(&context);
        GPUCompletionToken omittedReadToken;
        ASSERT_TRUE(InsertGPUCompletionPoint(omittedReadToken, omittedRead));
        // Lease acquisition alone is not sufficient: no realized graph access
        // was committed for this recording, so accepting the token would hide
        // an ownership/state handoff failure.
        omittedCommitUploader.NotifySubmission(omittedReadToken);
        EXPECT_EQ(omittedCommitUploader.GetDiagnostics().failureReason,
                  GPUSceneUploadFailureReason::InvalidCompletionToken);
        RenderGraph afterOmittedCommit;
        afterOmittedCommit.SetDevice(&device);
        EXPECT_FALSE(omittedCommitUploader.AcquireCurrentGraphLease(
            afterOmittedCommit, nullptr,
            omittedCommitDatabase.GetCommittedVersion()).has_value());
    }

    TEST(GPUSceneUploadValidation, LostDevicePreemptsTheWarmStaticFastPath)
    {
        FakeDevice device;
        RenderSubmissionTracker tracker;
        ASSERT_TRUE(tracker.Initialize(&device));
        GPUSceneUploader uploader;
        ASSERT_TRUE(uploader.Initialize(&device, &tracker));
        GPUSceneDatabase database;
        GPUSceneTransaction add;
        add.Add(MakeObject(1, 1.0F));
        ASSERT_TRUE(database.Commit(add).Succeeded());
        uploader.Observe(database.GetCommittedMirror(), database.GetLastChangeSet());

        FakeCommandContext context;
        RecordAndExecute(uploader, device, context);
        const GPUCompletionPoint point = tracker.Submit(&context);
        GPUCompletionToken token;
        ASSERT_TRUE(InsertGPUCompletionPoint(token, point));
        uploader.NotifySubmission(token);
        CompleteToken(device, token);
        ASSERT_EQ(uploader.GetDiagnostics().residentVersion,
                  database.GetCommittedVersion());

        device.SetDeviceLost(true);
        RenderGraph graph;
        graph.SetDevice(&device);
        uploader.BuildRenderGraph(graph, nullptr);
        EXPECT_TRUE(uploader.GetDiagnostics().deviceLost);
        EXPECT_EQ(uploader.GetDiagnostics().failureReason,
                  GPUSceneUploadFailureReason::DeviceLost);
    }

    TEST(GPUSceneUploadValidation, PendingV1ThenObservedV2RetainsTheLaterDelta)
    {
        FakeDevice device;
        RenderSubmissionTracker tracker;
        ASSERT_TRUE(tracker.Initialize(&device));
        GPUSceneUploader uploader;
        ASSERT_TRUE(uploader.Initialize(&device, &tracker));
        GPUSceneDatabase database;
        GPUSceneTransaction add;
        add.Add(MakeObject(1, 1.0F));
        ASSERT_TRUE(database.Commit(add).Succeeded());
        uploader.Observe(database.GetCommittedMirror(), database.GetLastChangeSet());
        FakeCommandContext context;
        RecordAndExecute(uploader, device, context); // V1 remains pending.

        const GPUScenePrimitiveRef primitive = database.FindPrimitive(1).value();
        GPUSceneTransaction update;
        update.Update(primitive, MakeObject(1, 9.0F));
        ASSERT_TRUE(database.Commit(update).Succeeded());
        uploader.Observe(database.GetCommittedMirror(), database.GetLastChangeSet());

        const GPUCompletionPoint point = tracker.Submit(&context);
        GPUCompletionToken token;
        ASSERT_TRUE(InsertGPUCompletionPoint(token, point));
        uploader.NotifySubmission(token);
        CompleteToken(device, token);

        context.copyCount = 0;
        RecordAndExecute(uploader, device, context);
        EXPECT_GT(context.copyCount, 0U);
        EXPECT_GT(uploader.GetDiagnostics().frameUploadBytes, 0U);
    }

    TEST(GPUSceneUploadValidation, ContinuousDeltasCoalesceAndPreserveUntouchedTables)
    {
        FakeDevice device;
        RenderSubmissionTracker tracker;
        ASSERT_TRUE(tracker.Initialize(&device));
        GPUSceneUploader uploader;
        ASSERT_TRUE(uploader.Initialize(&device, &tracker));
        GPUSceneDatabase database;
        GPUSceneTransaction add;
        add.Add(MakeObject(1, 1.0F));
        ASSERT_TRUE(database.Commit(add).Succeeded());
        uploader.Observe(database.GetCommittedMirror(), database.GetLastChangeSet());
        FakeCommandContext context;
        RecordAndExecute(uploader, device, context);
        const GPUCompletionPoint initialPoint = tracker.Submit(&context);
        GPUCompletionToken initialToken;
        ASSERT_TRUE(InsertGPUCompletionPoint(initialToken, initialPoint));
        uploader.NotifySubmission(initialToken);
        CompleteToken(device, initialToken);

        const GPUScenePrimitiveRef primitive = database.FindPrimitive(1).value();
        GPUSceneTransaction v2;
        v2.Update(primitive, MakeObject(1, 2.0F));
        ASSERT_TRUE(database.Commit(v2).Succeeded());
        uploader.Observe(database.GetCommittedMirror(), database.GetLastChangeSet());
        GPUSceneTransaction v3;
        v3.Update(primitive, MakeObject(1, 3.0F));
        ASSERT_TRUE(database.Commit(v3).Succeeded());
        uploader.Observe(database.GetCommittedMirror(), database.GetLastChangeSet());

        FakeBuffer* bounds = device.Find("GPUScene.Bounds");
        ASSERT_NE(bounds, nullptr);
        RecordAndExecute(uploader, device, context);
        ASSERT_GE(bounds->Bytes().size(),
                  database.GetCommittedMirror().bounds.size() * sizeof(GPUSceneBoundsRow));
        EXPECT_EQ(0, std::memcmp(bounds->Bytes().data(),
                                 database.GetCommittedMirror().bounds.data(),
                                 database.GetCommittedMirror().bounds.size() *
                                     sizeof(GPUSceneBoundsRow)));
    }

    TEST(GPUSceneUploadValidation, CapacityGrowthAndMissedBaseVersionForceFullInitialization)
    {
        FakeDevice device;
        RenderSubmissionTracker tracker;
        ASSERT_TRUE(tracker.Initialize(&device));
        GPUSceneUploader uploader;
        ASSERT_TRUE(uploader.Initialize(&device, &tracker));
        GPUSceneDatabase database;
        GPUSceneTransaction v1;
        v1.Add(MakeObject(1, 1.0F));
        ASSERT_TRUE(database.Commit(v1).Succeeded());
        uploader.Observe(database.GetCommittedMirror(), database.GetLastChangeSet());
        FakeCommandContext context;
        RecordAndExecute(uploader, device, context);
        const GPUCompletionPoint point = tracker.Submit(&context);
        GPUCompletionToken token;
        ASSERT_TRUE(InsertGPUCompletionPoint(token, point));
        uploader.NotifySubmission(token);
        CompleteToken(device, token);
        const uint32 beforeGrowth = device.DefaultBufferCount();

        GPUSceneTransaction v2;
        v2.Add(MakeObject(2, 2.0F)).Add(MakeObject(3, 3.0F));
        ASSERT_TRUE(database.Commit(v2).Succeeded());
        // Simulate a skipped observer generation before receiving V3.
        GPUSceneTransaction v3;
        v3.Update(database.FindPrimitive(1).value(), MakeObject(1, 4.0F));
        ASSERT_TRUE(database.Commit(v3).Succeeded());
        uploader.Observe(database.GetCommittedMirror(), database.GetLastChangeSet());
        RecordAndExecute(uploader, device, context);
        EXPECT_TRUE(uploader.GetDiagnostics().continuityLost);
        EXPECT_TRUE(uploader.GetDiagnostics().fullUpload);
        EXPECT_GE(device.DefaultBufferCount(), beforeGrowth + 6U);
        for (const char* name : {"GPUScene.Primitives", "GPUScene.Bounds", "GPUScene.Transforms",
                                 "GPUScene.Materials", "GPUScene.Geometries", "GPUScene.Draws"})
        {
            FakeBuffer* table = device.Find(name);
            ASSERT_NE(table, nullptr);
            EXPECT_EQ(0U, table->MapCalls());
        }
    }

    TEST(GPUSceneUploadValidation, MultiDomainCompletionAndFutureReadUseBlockReuseUntilAllDomainsComplete)
    {
        FakeDevice device;
        RenderSubmissionTracker tracker;
        ASSERT_TRUE(tracker.Initialize(&device));
        GPUSceneUploader uploader;
        ASSERT_TRUE(uploader.Initialize(&device, &tracker));
        GPUSceneDatabase database;
        GPUSceneTransaction add;
        add.Add(MakeObject(1, 1.0F));
        ASSERT_TRUE(database.Commit(add).Succeeded());
        uploader.Observe(database.GetCommittedMirror(), database.GetLastChangeSet());
        FakeCommandContext graphics;
        RecordAndExecute(uploader, device, graphics);
        FakeCommandContext compute(RHICommandQueueType::Compute);
        FakeCommandContext copy(RHICommandQueueType::Copy);
        const std::array<FakeCommandContext*, 3> contexts = {&graphics, &compute, &copy};
        const GPUCompletionToken token = SubmitToken(tracker, contexts);
        uploader.NotifySubmission(token);
        ASSERT_TRUE(RecordExactLeaseRead(
            uploader, device, graphics, database.GetCommittedVersion()));
        uploader.NotifySubmission(token); // Merge a same-version future read.

        device.Fence(0)->Complete(token.points[0].value);
        EXPECT_EQ(uploader.PollSafeReclaimVersion(), 0U);
        const uint32 before = device.DefaultBufferCount();
        GPUSceneTransaction update;
        update.Update(database.FindPrimitive(1).value(), MakeObject(1, 5.0F));
        ASSERT_TRUE(database.Commit(update).Succeeded());
        uploader.Observe(database.GetCommittedMirror(), database.GetLastChangeSet());
        RenderGraph graph;
        graph.SetDevice(&device);
        uploader.BuildRenderGraph(graph, nullptr);
        EXPECT_GE(device.DefaultBufferCount(), before + 6U);
        uploader.ReleaseUnsubmittedFrame();

        device.Fence(1)->Complete(token.points[1].value);
        EXPECT_EQ(uploader.PollSafeReclaimVersion(), 0U);
        device.Fence(2)->Complete(token.points[2].value);
        EXPECT_GE(uploader.PollSafeReclaimVersion(), database.GetCommittedVersion() - 1U);
    }

    TEST(GPUSceneUploadValidation, InvalidTokenAndResourceFailuresFailClosedWithoutDefaultMapping)
    {
        FakeDevice device;
        RenderSubmissionTracker tracker;
        ASSERT_TRUE(tracker.Initialize(&device));
        GPUSceneUploader uploader;
        ASSERT_TRUE(uploader.Initialize(&device, &tracker));
        GPUSceneDatabase database;
        GPUSceneTransaction add;
        add.Add(MakeObject(1, 1.0F));
        ASSERT_TRUE(database.Commit(add).Succeeded());
        uploader.Observe(database.GetCommittedMirror(), database.GetLastChangeSet());
        FakeCommandContext context;
        RecordAndExecute(uploader, device, context);
        GPUCompletionToken invalid;
        invalid.count = 1;
        invalid.points[0] = {GPUQueueDomain::Graphics, 0};
        uploader.NotifySubmission(invalid);
        EXPECT_EQ(uploader.GetDiagnostics().failureReason,
                  GPUSceneUploadFailureReason::InvalidCompletionToken);

        GPUSceneUploader failedUploader;
        ASSERT_TRUE(failedUploader.Initialize(&device, &tracker));
        GPUSceneDatabase failedDatabase;
        GPUSceneTransaction failedAdd;
        failedAdd.Add(MakeObject(2, 2.0F));
        ASSERT_TRUE(failedDatabase.Commit(failedAdd).Succeeded());
        failedUploader.Observe(failedDatabase.GetCommittedMirror(),
                               failedDatabase.GetLastChangeSet());
        device.FailCreateAfter(0);
        RenderGraph graph;
        graph.SetDevice(&device);
        failedUploader.BuildRenderGraph(graph, nullptr);
        EXPECT_EQ(failedUploader.GetDiagnostics().failureReason,
                  GPUSceneUploadFailureReason::BufferCreationFailed);

        FakeDevice stagingDevice;
        RenderSubmissionTracker stagingTracker;
        ASSERT_TRUE(stagingTracker.Initialize(&stagingDevice));
        GPUSceneUploader stagingUploader;
        ASSERT_TRUE(stagingUploader.Initialize(&stagingDevice, &stagingTracker));
        stagingUploader.Observe(failedDatabase.GetCommittedMirror(),
                                failedDatabase.GetLastChangeSet());
        stagingDevice.FailCreateAfter(6); // six persistent tables, then staging
        RenderGraph stagingGraph;
        stagingGraph.SetDevice(&stagingDevice);
        stagingUploader.BuildRenderGraph(stagingGraph, nullptr);
        EXPECT_EQ(stagingUploader.GetDiagnostics().failureReason,
                  GPUSceneUploadFailureReason::StagingCreationFailed);

        device.SetFailUploadMap(true);
        failedUploader.BuildRenderGraph(graph, nullptr);
        EXPECT_EQ(failedUploader.GetDiagnostics().failureReason,
                  GPUSceneUploadFailureReason::StagingMapFailed);
        for (const char* name : {"GPUScene.Primitives", "GPUScene.Bounds", "GPUScene.Transforms",
                                 "GPUScene.Materials", "GPUScene.Geometries", "GPUScene.Draws"})
        {
            if (FakeBuffer* table = device.Find(name))
            {
                EXPECT_EQ(table->MapCalls(), 0U);
            }
        }
    }

    TEST(GPUSceneUploadValidation, UnissuedOrInactiveCompletionTokensDoNotAdvanceResidency)
    {
        FakeDevice compatibilityDevice(true);
        RenderSubmissionTracker compatibilityTracker;
        ASSERT_TRUE(compatibilityTracker.Initialize(&compatibilityDevice));
        GPUSceneUploader inactiveUploader;
        ASSERT_TRUE(inactiveUploader.Initialize(&compatibilityDevice, &compatibilityTracker));
        GPUSceneDatabase inactiveDatabase;
        GPUSceneTransaction inactiveAdd;
        inactiveAdd.Add(MakeObject(1, 1.0F));
        ASSERT_TRUE(inactiveDatabase.Commit(inactiveAdd).Succeeded());
        inactiveUploader.Observe(inactiveDatabase.GetCommittedMirror(),
                                 inactiveDatabase.GetLastChangeSet());
        FakeCommandContext context;
        RecordAndExecute(inactiveUploader, compatibilityDevice, context);
        GPUCompletionToken inactiveToken;
        ASSERT_TRUE(InsertGPUCompletionPoint(
            inactiveToken, {GPUQueueDomain::Compute, 1}));
        inactiveUploader.NotifySubmission(inactiveToken);
        EXPECT_EQ(inactiveUploader.GetDiagnostics().failureReason,
                  GPUSceneUploadFailureReason::InvalidCompletionToken);
        EXPECT_EQ(inactiveUploader.GetDiagnostics().residentVersion, 0U);

        FakeDevice device;
        RenderSubmissionTracker tracker;
        ASSERT_TRUE(tracker.Initialize(&device));
        GPUSceneUploader futureUploader;
        ASSERT_TRUE(futureUploader.Initialize(&device, &tracker));
        GPUSceneDatabase futureDatabase;
        GPUSceneTransaction futureAdd;
        futureAdd.Add(MakeObject(2, 2.0F));
        ASSERT_TRUE(futureDatabase.Commit(futureAdd).Succeeded());
        futureUploader.Observe(futureDatabase.GetCommittedMirror(),
                               futureDatabase.GetLastChangeSet());
        RecordAndExecute(futureUploader, device, context);
        const GPUCompletionPoint submitted = tracker.Submit(&context);
        ASSERT_NE(submitted.value, 0U);
        GPUCompletionToken futureToken;
        ASSERT_TRUE(InsertGPUCompletionPoint(
            futureToken, {submitted.domain, submitted.value + 1U}));
        futureUploader.NotifySubmission(futureToken);
        EXPECT_EQ(futureUploader.GetDiagnostics().failureReason,
                  GPUSceneUploadFailureReason::InvalidCompletionToken);
        EXPECT_EQ(futureUploader.GetDiagnostics().residentVersion, 0U);
    }

    TEST(GPUSceneUploadValidation, RepeatedFutureReadsMergeToTheLatestSubmissionToken)
    {
        FakeDevice device;
        RenderSubmissionTracker tracker;
        ASSERT_TRUE(tracker.Initialize(&device));
        GPUSceneUploader uploader;
        ASSERT_TRUE(uploader.Initialize(&device, &tracker));
        GPUSceneDatabase database;
        GPUSceneTransaction add;
        add.Add(MakeObject(1, 1.0F));
        ASSERT_TRUE(database.Commit(add).Succeeded());
        uploader.Observe(database.GetCommittedMirror(), database.GetLastChangeSet());
        FakeCommandContext context;
        RecordAndExecute(uploader, device, context);
        const GPUCompletionPoint uploaded = tracker.Submit(&context);
        GPUCompletionToken uploadedToken;
        ASSERT_TRUE(InsertGPUCompletionPoint(uploadedToken, uploaded));
        uploader.NotifySubmission(uploadedToken);
        CompleteToken(device, uploadedToken);

        ASSERT_TRUE(RecordExactLeaseRead(
            uploader, device, context, database.GetCommittedVersion()));
        const GPUCompletionPoint firstRead = tracker.Submit(&context);
        GPUCompletionToken firstReadToken;
        ASSERT_TRUE(InsertGPUCompletionPoint(firstReadToken, firstRead));
        uploader.NotifySubmission(firstReadToken);
        ASSERT_TRUE(RecordExactLeaseRead(
            uploader, device, context, database.GetCommittedVersion()));
        const GPUCompletionPoint secondRead = tracker.Submit(&context);
        GPUCompletionToken secondReadToken;
        ASSERT_TRUE(InsertGPUCompletionPoint(secondReadToken, secondRead));
        uploader.NotifySubmission(secondReadToken);

        device.Fence(0)->Complete(firstRead.value);
        EXPECT_EQ(uploader.PollSafeReclaimVersion(), 0U);
        device.Fence(0)->Complete(secondRead.value);
        EXPECT_GE(uploader.PollSafeReclaimVersion(), database.GetCommittedVersion());
    }

    TEST(GPUSceneUploadValidation, CompatibilityWaitIdleAndDeviceLostDoNotPermitUnsafeReuse)
    {
        FakeDevice compatibilityDevice(true);
        RenderSubmissionTracker compatibilityTracker;
        ASSERT_TRUE(compatibilityTracker.Initialize(&compatibilityDevice));
        GPUSceneUploader compatibilityUploader;
        ASSERT_TRUE(compatibilityUploader.Initialize(&compatibilityDevice, &compatibilityTracker));
        GPUSceneDatabase database;
        GPUSceneTransaction add;
        add.Add(MakeObject(1, 1.0F));
        ASSERT_TRUE(database.Commit(add).Succeeded());
        compatibilityUploader.Observe(database.GetCommittedMirror(), database.GetLastChangeSet());
        FakeCommandContext context;
        RecordAndExecute(compatibilityUploader, compatibilityDevice, context);
        const GPUCompletionPoint point = compatibilityTracker.Submit(&context);
        GPUCompletionToken token;
        ASSERT_TRUE(InsertGPUCompletionPoint(token, point));
        compatibilityUploader.NotifySubmission(token);
        EXPECT_EQ(compatibilityUploader.PollSafeReclaimVersion(), 0U);
        EXPECT_EQ(compatibilityTracker.Wait(token), GPUCompletionStatus::CompatibilityWaitIdle);
        EXPECT_GT(compatibilityUploader.PollSafeReclaimVersion(), 0U);

        FakeDevice device;
        RenderSubmissionTracker tracker;
        ASSERT_TRUE(tracker.Initialize(&device));
        GPUSceneUploader uploader;
        ASSERT_TRUE(uploader.Initialize(&device, &tracker));
        uploader.Observe(database.GetCommittedMirror(), database.GetLastChangeSet());
        device.SetDeviceLost(true);
        RenderGraph graph;
        graph.SetDevice(&device);
        uploader.BuildRenderGraph(graph, nullptr);
        EXPECT_TRUE(uploader.GetDiagnostics().deviceLost ||
                    uploader.GetDiagnostics().failureReason ==
                        GPUSceneUploadFailureReason::DeviceLost);
    }

    TEST(GPUSceneUploadValidation,
         DiagnosticsReportExactUploadAllocationAndWarmStaticZeroCopy)
    {
        FakeDevice device;
        RenderSubmissionTracker tracker;
        ASSERT_TRUE(tracker.Initialize(&device));
        GPUSceneUploader uploader;
        ASSERT_TRUE(uploader.Initialize(&device, &tracker));
        GPUSceneDatabase database;
        GPUSceneTransaction add;
        add.Add(MakeObject(41, 1.0F));
        ASSERT_TRUE(database.Commit(add).Succeeded());
        uploader.Observe(database.GetCommittedMirror(), database.GetLastChangeSet());

        const GPUSceneUploadDiagnostics beforeUpload = uploader.GetDiagnostics();
        EXPECT_GT(beforeUpload.cpuPayloadBytes, 0U);
        EXPECT_EQ(beforeUpload.gpuAllocationBytes, 0U);
        EXPECT_EQ(beforeUpload.frameUploadBytes, 0U);

        FakeCommandContext context;
        RecordAndExecute(uploader, device, context);
        const GPUSceneUploadDiagnostics recorded = uploader.GetDiagnostics();
        EXPECT_GT(recorded.frameUploadBytes, 0U);
        EXPECT_EQ(recorded.frameUploadBytes, recorded.cumulativeUploadBytes);
        EXPECT_EQ(recorded.frameUploadBytes, recorded.peakFrameUploadBytes);
        EXPECT_GT(recorded.gpuAllocationBytes, 0U);
        EXPECT_EQ(recorded.bufferSetCount, 1U);
        EXPECT_TRUE(recorded.fullUpload);
        for (const GPUSceneTableDiagnostics& table : recorded.tables)
        {
            EXPECT_GT(table.frameUploadBytes, 0U);
            EXPECT_TRUE(table.fullUpload);
        }

        const GPUCompletionPoint submitted = tracker.Submit(&context);
        GPUCompletionToken token;
        ASSERT_TRUE(InsertGPUCompletionPoint(token, submitted));
        uploader.NotifySubmission(token);
        CompleteToken(device, token);
        ASSERT_GE(uploader.PollSafeReclaimVersion(), database.GetCommittedVersion());

        RenderGraph warmGraph;
        warmGraph.SetDevice(&device);
        uploader.BuildRenderGraph(warmGraph, nullptr);
        warmGraph.Compile();
        ASSERT_TRUE(warmGraph.GetCompileStats().compileValid);
        const GPUSceneUploadDiagnostics warm = uploader.GetDiagnostics();
        EXPECT_EQ(warm.frameUploadBytes, 0U);
        EXPECT_EQ(warm.frameUploadRangeCount, 0U);
        EXPECT_EQ(warm.cumulativeUploadBytes, recorded.cumulativeUploadBytes);
        EXPECT_GE(warm.peakFrameUploadBytes, recorded.peakFrameUploadBytes);
        EXPECT_EQ(warm.tables[static_cast<uint32>(GPUSceneDiagnosticsTable::Primitives)].
                      residentCapacity,
                  2U);
        EXPECT_FALSE(warm.executionEligible);
    }
} // namespace
