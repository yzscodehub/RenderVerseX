#include "GPUScene/GPUSceneUploader.h"
#include "../Common/RenderGraphValidationAccess.h"
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
        explicit FakeBuffer(const RHIBufferDesc& desc,
                            uint32* uploadCommitCalls = nullptr)
            : m_desc(desc),
              m_bytes(static_cast<size_t>(desc.size)),
              m_uploadCommitCalls(uploadCommitCalls)
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
        bool CommitMappedWrite() override
        {
            ++m_commitCalls;
            if (m_uploadCommitCalls != nullptr)
            {
                ++*m_uploadCommitCalls;
            }
            return !m_failCommit;
        }
        void SetCommitFailure(bool enabled) { m_failCommit = enabled; }

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
        uint32 CommitCalls() const { return m_commitCalls; }

    private:
        RHIBufferDesc m_desc;
        std::vector<uint8> m_bytes;
        uint32 m_mapCalls = 0;
        uint32 m_commitCalls = 0;
        uint32* m_uploadCommitCalls = nullptr;
        bool m_failMap = false;
        bool m_failCommit = false;
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
        void BufferBarrier(const RHIBufferBarrier& barrier) override
        {
            bufferBarriers.push_back(barrier);
        }
        void TextureBarrier(const RHITextureBarrier&) override {}
        void Barriers(std::span<const RHIBufferBarrier> buffers,
                      std::span<const RHITextureBarrier>) override
        {
            bufferBarriers.insert(bufferBarriers.end(),
                                  buffers.begin(),
                                  buffers.end());
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
        std::vector<RHIBufferBarrier> bufferBarriers;
    private:
        RHICommandQueueType m_queue = RHICommandQueueType::Graphics;
    };

    class FakeDevice final : public IRHIDevice
    {
    public:
        explicit FakeDevice(bool compatibility = false)
        {
            m_backendType = compatibility
                ? RHIBackendType::DX11
                : RHIBackendType::DX12;
            m_capabilities.backendType = m_backendType;
            m_capabilities.adapterName = "GPUSceneUploadFake";
            m_capabilities.driverVersion = "1";
            m_capabilities.supportsComputePipeline = true;
            m_capabilities.supportsDescriptorSets = true;
            m_capabilities.supportsDynamicDescriptorOffsets = true;
            m_capabilities.maxDescriptorSets = 4;
            m_capabilities.supportsExplicitResourceBarriers = true;
            m_capabilities.supportsDefaultQueueFenceSignal = !compatibility;
            m_capabilities.supportsExplicitQueueFenceSignal = !compatibility;
            m_capabilities.supportsQueueFenceWait = !compatibility;
            m_capabilities.supportsMultiQueueBatchSubmit = !compatibility;
            m_capabilities.supportsQueueSubmissionPlan = !compatibility;
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
            RHIBufferRef result(new FakeBuffer(
                desc,
                desc.memoryType == RHIMemoryType::Upload
                    ? &m_uploadCommitCalls
                    : nullptr));
            if (m_failUploadMap && desc.memoryType == RHIMemoryType::Upload)
            {
                static_cast<FakeBuffer*>(result.Get())->SetMapFailure(true);
            }
            if (desc.memoryType == RHIMemoryType::Upload)
            {
                if (m_failUploadCommitAfter == 0)
                {
                    static_cast<FakeBuffer*>(result.Get())->SetCommitFailure(true);
                    m_failUploadCommitAfter = -1;
                }
                else if (m_failUploadCommitAfter > 0)
                {
                    --m_failUploadCommitAfter;
                }
            }
            // Retain every created fake buffer because allocation-history
            // assertions and Find() intentionally inspect objects after the
            // production owner releases transient staging references.
            m_buffers.push_back(result);
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
        RHICommandContextRef CreateCommandContext(
            RHICommandQueueType type) override
        {
            return RHICommandContextRef(new FakeCommandContext(type));
        }
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
            return m_backendType;
        }
        RHIDeviceRuntimeStatus QueryRuntimeStatus() const noexcept override
        {
            return m_deviceLost ? RHIDeviceRuntimeStatus::DeviceLost
                                : RHIDeviceRuntimeStatus::Ready;
        }

        FakeBuffer* Find(const char* name,
                         RHIMemoryType memoryType = RHIMemoryType::Default) const
        {
            for (const RHIBufferRef& bufferOwner : m_buffers)
            {
                auto* buffer = static_cast<FakeBuffer*>(bufferOwner.Get());
                if (buffer && buffer->GetDebugName() == name &&
                    buffer->GetMemoryType() == memoryType)
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
            for (const RHIBufferRef& bufferOwner : m_buffers)
            {
                const auto* buffer = static_cast<const FakeBuffer*>(bufferOwner.Get());
                count += buffer && buffer->GetMemoryType() == RHIMemoryType::Default;
            }
            return count;
        }
        uint32 UploadCommitCalls() const
        {
            return m_uploadCommitCalls;
        }
        void FailCreateAfter(int32 calls) { m_createFailureCountdown = calls; }
        void SetFailUploadMap(bool enabled) { m_failUploadMap = enabled; }
        void FailUploadCommitAfter(int32 calls) { m_failUploadCommitAfter = calls; }
        void SetDeviceLost(bool lost) { m_deviceLost = lost; }
        void SetCopyQueueDomain(GPUQueueDomain domain)
        {
            m_capabilities.queueTopology.logicalQueueDomains[
                static_cast<uint8>(RHICommandQueueType::Copy)] = domain;
            m_capabilities.queueTopology.activeDomainCount =
                domain == GPUQueueDomain::Copy ? 3 : 2;
        }
        void SetQueueSubmissionPlanSupported(bool supported)
        {
            m_capabilities.supportsQueueSubmissionPlan = supported;
        }
        void SetBackendType(RHIBackendType backendType)
        {
            m_backendType = backendType;
            m_capabilities.backendType = backendType;
            if (backendType == RHIBackendType::Vulkan)
            {
                m_capabilities.vulkan.apiVersion = 1;
            }
        }

    private:
        RHICapabilities m_capabilities;
        std::vector<RHIBufferRef> m_buffers;
        std::vector<RHIFenceRef> m_fences;
        int32 m_createFailureCountdown = -1;
        int32 m_failUploadCommitAfter = -1;
        uint32 m_uploadCommitCalls = 0;
        bool m_failUploadMap = false;
        bool m_deviceLost = false;
        RHIBackendType m_backendType = RHIBackendType::DX12;
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
        // This helper intentionally exercises the GraphicsOnly recorder. Its
        // fake capability snapshot must therefore disable queue plans before
        // GPUScene records the staging import; the production multi-queue
        // behavior is covered explicitly by UploadStagingUsesCopyDomain...
        const bool queueSubmissionPlanSupported =
            device.GetCapabilities().supportsQueueSubmissionPlan;
        device.SetQueueSubmissionPlanSupported(false);
        RenderGraph graph;
        RenderGraphValidationAccess::SetDevice(graph, &device);
        uploader.BuildRenderGraph(graph, nullptr);
        RenderGraphValidationAccess::Compile(graph);
        ASSERT_TRUE(graph.GetCompileStats().compileValid);
        RenderGraphValidationAccess::Execute(graph, context);
        uploader.CommitRealizedAccess(graph);
        device.SetQueueSubmissionPlanSupported(queueSubmissionPlanSupported);
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
        RenderGraphValidationAccess::SetDevice(graph, &device);
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
        RenderGraphValidationAccess::Compile(graph);
        if (!graph.GetCompileStats().compileValid)
        {
            return false;
        }
        RenderGraphValidationAccess::Execute(graph, context);
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

    bool HasOwnershipTransition(
        const RenderGraph::RecordedQueueSubmission& recorded,
        const RHIBuffer* buffer,
        GPUQueueDomain before,
        GPUQueueDomain after)
    {
        for (const RHICommandContextRef& context : recorded.ownedContexts)
        {
            const auto* fakeContext = dynamic_cast<const FakeCommandContext*>(
                context.Get());
            if (!fakeContext)
            {
                continue;
            }

            const bool found = std::any_of(
                fakeContext->bufferBarriers.begin(),
                fakeContext->bufferBarriers.end(),
                [buffer, before, after](const RHIBufferBarrier& barrier)
                {
                    return barrier.buffer == buffer &&
                           barrier.accessBefore.domain == before &&
                           barrier.accessAfter.domain == after &&
                           HasDependencyKind(
                               barrier.dependencyKind,
                               RHIDependencyKind::Ownership);
                });
            if (found)
            {
                return true;
            }
        }
        return false;
    }

    bool HasAnyOwnershipTransition(
        const RenderGraph::RecordedQueueSubmission& recorded,
        const RHIBuffer* buffer)
    {
        for (const RHICommandContextRef& context : recorded.ownedContexts)
        {
            const auto* fakeContext = dynamic_cast<const FakeCommandContext*>(
                context.Get());
            if (!fakeContext)
            {
                continue;
            }

            const bool found = std::any_of(
                fakeContext->bufferBarriers.begin(),
                fakeContext->bufferBarriers.end(),
                [buffer](const RHIBufferBarrier& barrier)
                {
                    return barrier.buffer == buffer &&
                           HasDependencyKind(
                               barrier.dependencyKind,
                               RHIDependencyKind::Ownership);
                });
            if (found)
            {
                return true;
            }
        }
        return false;
    }

    bool HasAnyOwnershipTransition(const FakeCommandContext& context,
                                   const RHIBuffer* buffer)
    {
        return std::any_of(
            context.bufferBarriers.begin(),
            context.bufferBarriers.end(),
            [buffer](const RHIBufferBarrier& barrier)
            {
                return barrier.buffer == buffer &&
                       HasDependencyKind(barrier.dependencyKind,
                                         RHIDependencyKind::Ownership);
            });
    }

    TEST(GPUSceneUploadValidation,
         UploadStagingUsesCopyDomainWithoutFabricatingGraphicsOwnership)
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

        // BuildRenderGraph runs before the final submission plan is compiled.
        // The final plan nevertheless selects the dedicated Copy domain.
        RenderGraph graph;
        RenderGraphValidationAccess::SetDevice(graph, &device);
        uploader.BuildRenderGraph(graph, nullptr);
        RenderGraphCompileOptions options;
        options.queuePolicy = RGQueuePolicy::PreferMultiQueue;
        options.capabilities = device.GetCapabilities();
        options.hasCapabilitySnapshot = true;
        RenderGraphValidationAccess::Compile(graph, options);
        ASSERT_TRUE(graph.GetCompileStats().compileValid);
        ASSERT_EQ(graph.GetQueueExecutionMode(),
                  RenderGraph::QueueExecutionMode::MultiQueue);

        RenderGraph::RecordedQueueSubmission recorded;
        ASSERT_TRUE(RenderGraphValidationAccess::RecordQueueSubmission(
            graph, recorded));
        const FakeBuffer* staging = device.Find(
            "GPUScene.UploadStaging", RHIMemoryType::Upload);
        const FakeBuffer* target = device.Find("GPUScene.Primitives");
        ASSERT_NE(staging, nullptr);
        ASSERT_NE(target, nullptr);

        // CPU-written staging enters its first and only GPU use on Copy: no
        // fabricated external Graphics->Copy release/acquire is allowed.
        EXPECT_FALSE(HasOwnershipTransition(
            recorded, staging, GPUQueueDomain::Graphics, GPUQueueDomain::Copy));
        EXPECT_FALSE(HasAnyOwnershipTransition(recorded, staging));

        // The persistent table is different: it is produced on Copy and is
        // exported to Graphics for its later ShaderResource consumers.
        EXPECT_TRUE(HasOwnershipTransition(
            recorded, target, GPUQueueDomain::Copy, GPUQueueDomain::Graphics));
        uploader.ReleaseUnsubmittedFrame();
    }

    TEST(GPUSceneUploadValidation,
         UploadStagingFallsBackToGraphicsForAliasedOrUnavailableCopyPlans)
    {
        const auto expectNoStagingOwnership = [](FakeDevice& device)
        {
            RenderSubmissionTracker tracker;
            ASSERT_TRUE(tracker.Initialize(&device));
            GPUSceneUploader uploader;
            ASSERT_TRUE(uploader.Initialize(&device, &tracker));
            GPUSceneDatabase database;
            GPUSceneTransaction add;
            add.Add(MakeObject(1, 1.0F));
            ASSERT_TRUE(database.Commit(add).Succeeded());
            uploader.Observe(database.GetCommittedMirror(),
                             database.GetLastChangeSet());

            RenderGraph graph;
            RenderGraphValidationAccess::SetDevice(graph, &device);
            uploader.BuildRenderGraph(graph, nullptr);
            RenderGraphCompileOptions options;
            options.queuePolicy = RGQueuePolicy::PreferMultiQueue;
            options.capabilities = device.GetCapabilities();
            options.hasCapabilitySnapshot = true;
            RenderGraphValidationAccess::Compile(graph, options);
            ASSERT_TRUE(graph.GetCompileStats().compileValid);

            // Both cases resolve the Copy pass to Graphics, so the graphics
            // recorder is the relevant execution path. The no-plan case must
            // not attempt to construct a multi-queue submission at all.
            FakeCommandContext graphicsContext;
            RenderGraphValidationAccess::Execute(graph, graphicsContext);
            EXPECT_GT(graphicsContext.copyCount, 0U);
            const FakeBuffer* staging = device.Find(
                "GPUScene.UploadStaging", RHIMemoryType::Upload);
            ASSERT_NE(staging, nullptr);
            EXPECT_FALSE(HasAnyOwnershipTransition(graphicsContext, staging));
            uploader.ReleaseUnsubmittedFrame();
        };

        // Vulkan may legally alias its logical Copy queue to Graphics.
        FakeDevice aliasedCopyDevice;
        aliasedCopyDevice.SetBackendType(RHIBackendType::Vulkan);
        aliasedCopyDevice.SetCopyQueueDomain(GPUQueueDomain::Graphics);
        expectNoStagingOwnership(aliasedCopyDevice);

        // A distinct topology alone is insufficient: without an executable
        // queue submission plan RenderGraph routes Copy passes on Graphics.
        FakeDevice noPlanDevice;
        noPlanDevice.SetQueueSubmissionPlanSupported(false);
        expectNoStagingOwnership(noPlanDevice);
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
        // Recording and CPU staging evidence are not GPU-copy publication.
        EXPECT_EQ(uploader.GetDiagnostics().frameUploadBytes, 0U);
        EXPECT_EQ(uploader.GetDiagnostics().frameUploadRangeCount, 0U);
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
        const GPUSceneUploadMutationTotals fullMutationTotals =
            uploader.GetDiagnostics().mutationTotals;
        for (uint32 tableIndex = 0;
             tableIndex < GPU_SCENE_RESIDENT_TABLE_COUNT;
             ++tableIndex)
        {
            // Full materialization writes the exact allocated resident range,
            // including capacity slack beyond the payload's sentinel/object rows.
            EXPECT_EQ(fullMutationTotals
                          .submittedUploadedRowCount[tableIndex],
                      uploader.GetDiagnostics().tables[tableIndex]
                          .residentCapacity);
        }

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

        EXPECT_EQ(uploader.GetDiagnostics().frameUploadBytes, 0U);
        EXPECT_EQ(uploader.GetDiagnostics().frameUploadRangeCount, 0U);
        const GPUCompletionPoint incrementalUpload = tracker.Submit(&context);
        ASSERT_NE(incrementalUpload.value, 0U);
        GPUCompletionToken incrementalToken;
        ASSERT_TRUE(InsertGPUCompletionPoint(incrementalToken, incrementalUpload));
        uploader.NotifySubmission(incrementalToken);

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
        for (uint32 tableIndex = 0;
             tableIndex < GPU_SCENE_RESIDENT_TABLE_COUNT;
             ++tableIndex)
        {
            // The durable row total exposes a hidden population-sized rewrite:
            // only the 1% dirty rows may advance after the initial full upload.
            EXPECT_EQ(diagnostics.mutationTotals
                          .submittedUploadedRowCount[tableIndex],
                      static_cast<uint64>(fullMutationTotals
                                               .submittedUploadedRowCount[tableIndex]) +
                          dirtyCount);
        }
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
        RenderGraphValidationAccess::SetDevice(graph, &device);
        uploader.BuildRenderGraph(graph, nullptr);
        RenderGraphValidationAccess::Compile(graph);
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
        RenderGraphValidationAccess::SetDevice(graph, &device);
        uploader.BuildRenderGraph(graph, nullptr);
        RenderGraphValidationAccess::Compile(graph);
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
        RenderGraphValidationAccess::SetDevice(graph, &device);
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
        RenderGraphValidationAccess::SetDevice(graph, &device);
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
        const RGBufferHandle candidateHandle = RenderGraphValidationAccess::ImportBuffer(graph,
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
        RenderGraphValidationAccess::Compile(graph);
        ASSERT_TRUE(graph.GetCompileStats().compileValid);
        EXPECT_EQ(3u, graph.GetCompileStats().totalPasses);
        RenderGraphValidationAccess::Execute(graph, context);
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
        RenderGraphValidationAccess::SetDevice(retryGraph, &device);
        EXPECT_TRUE(uploader.AcquireCurrentGraphLease(
            retryGraph, nullptr, database.GetCommittedVersion()).has_value());
        uploader.ReleaseUnsubmittedFrame();
        batch.ReleaseUnsubmitted(retirement);
    }

    TEST(GPUSceneUploadValidation,
         PersistentTableOwnershipSurvivesAFrameBoundaryWithoutRedundantAcquire)
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
        const uint64 version = database.GetCommittedVersion();
        uploader.Observe(database.GetCommittedMirror(),
                         database.GetLastChangeSet());

        FakeCommandContext uploadContext;
        RecordAndExecute(uploader, device, uploadContext);
        const GPUCompletionPoint uploadPoint = tracker.Submit(&uploadContext);
        ASSERT_NE(uploadPoint.value, 0U);
        GPUCompletionToken uploadToken;
        ASSERT_TRUE(InsertGPUCompletionPoint(uploadToken, uploadPoint));
        uploader.NotifySubmission(uploadToken);
        CompleteToken(device, uploadToken);

        struct ComputeReadData
        {
            std::array<RGBufferHandle, GPU_SCENE_RESIDENT_TABLE_COUNT> tables;
            RGBufferHandle sink;
        };
        struct GraphicsReadData
        {
            RGBufferHandle primitive;
            RGBufferHandle transform;
            RGBufferHandle sink;
        };
        const auto recordGPUSceneFrame = [&device](
            RenderGraph& graph,
            const GPUSceneResidentGraphLease& lease,
            RenderGraph::RecordedQueueSubmission& recorded)
        {
            RHIBufferDesc sinkDesc;
            sinkDesc.size = sizeof(uint32);
            sinkDesc.stride = sizeof(uint32);
            sinkDesc.usage = RHIBufferUsage::Structured |
                             RHIBufferUsage::UnorderedAccess;
            sinkDesc.memoryType = RHIMemoryType::Default;
            sinkDesc.debugName = "GPUScene.PersistentOwnershipSink";
            const RHIBufferRef computeSink = device.CreateBuffer(sinkDesc);
            const RHIBufferRef graphicsSink = device.CreateBuffer(sinkDesc);
            EXPECT_NE(computeSink, nullptr);
            EXPECT_NE(graphicsSink, nullptr);
            const RGBufferHandle computeSinkHandle = graph.ImportBuffer(
                computeSink,
                MakeRHIBufferAccessSnapshot(
                    RHIResourceState::Common,
                    RHIShaderStage::None,
                    GPUQueueDomain::Graphics,
                    RHIContentValidity::Valid));
            const RGBufferHandle graphicsSinkHandle = graph.ImportBuffer(
                graphicsSink,
                MakeRHIBufferAccessSnapshot(
                    RHIResourceState::Common,
                    RHIShaderStage::None,
                    GPUQueueDomain::Graphics,
                    RHIContentValidity::Valid));

            graph.AddPass<ComputeReadData>(
                "GPUScene.PersistentComputeRead",
                RenderGraphPassType::Compute,
                [lease, computeSinkHandle](RenderGraphBuilder& builder,
                                           ComputeReadData& data)
                {
                    for (uint32 tableIndex = 0;
                         tableIndex < GPU_SCENE_RESIDENT_TABLE_COUNT;
                         ++tableIndex)
                    {
                        data.tables[tableIndex] = builder.Read(
                            lease.handles[tableIndex],
                            MakeRGAccessDesc(
                                RHIResourceState::ShaderResource,
                                RHIShaderStage::Compute));
                    }
                    data.sink = builder.Write(
                        computeSinkHandle,
                        MakeRGAccessDesc(
                            RHIResourceState::UnorderedAccess,
                            RHIShaderStage::Compute));
                },
                [](const ComputeReadData&, RHICommandContext&) {});
            graph.AddPass<GraphicsReadData>(
                "GPUScene.PersistentGraphicsRead",
                RenderGraphPassType::Graphics,
                [lease, graphicsSinkHandle](RenderGraphBuilder& builder,
                                            GraphicsReadData& data)
                {
                    data.primitive = builder.Read(
                        lease.handles[static_cast<uint32>(
                            GPUSceneResidentTable::Primitives)],
                        MakeRGAccessDesc(
                            RHIResourceState::ShaderResource,
                            RHIShaderStage::Vertex));
                    data.transform = builder.Read(
                        lease.handles[static_cast<uint32>(
                            GPUSceneResidentTable::Transforms)],
                        MakeRGAccessDesc(
                            RHIResourceState::ShaderResource,
                            RHIShaderStage::Vertex));
                    data.sink = builder.Write(
                        graphicsSinkHandle,
                        MakeRGAccessDesc(
                            RHIResourceState::UnorderedAccess,
                            RHIShaderStage::Pixel));
                },
                [](const GraphicsReadData&, RHICommandContext&) {});

            RenderGraphCompileOptions options;
            options.queuePolicy = RGQueuePolicy::PreferMultiQueue;
            options.capabilities = device.GetCapabilities();
            options.hasCapabilitySnapshot = true;
            RenderGraphValidationAccess::Compile(graph, options);
            EXPECT_TRUE(graph.GetCompileStats().compileValid);
            EXPECT_EQ(graph.GetQueueExecutionMode(),
                      RenderGraph::QueueExecutionMode::MultiQueue);
            EXPECT_TRUE(RenderGraphValidationAccess::RecordQueueSubmission(
                graph, recorded));
        };

        RenderGraph firstGraph;
        RenderGraphValidationAccess::SetDevice(firstGraph, &device);
        const std::optional<GPUSceneResidentGraphLease> firstLease =
            uploader.AcquireCurrentGraphLease(firstGraph, nullptr, version);
        ASSERT_TRUE(firstLease.has_value());
        RenderGraph::RecordedQueueSubmission firstRecorded;
        recordGPUSceneFrame(firstGraph, *firstLease, firstRecorded);
        ASSERT_FALSE(firstRecorded.ownedContexts.empty());
        uploader.CommitRealizedAccess(firstGraph);

        FakeCommandContext terminalContext;
        const GPUCompletionPoint firstReadPoint = tracker.Submit(&terminalContext);
        ASSERT_NE(firstReadPoint.value, 0U);
        GPUCompletionToken firstReadToken;
        ASSERT_TRUE(InsertGPUCompletionPoint(firstReadToken, firstReadPoint));
        uploader.NotifySubmission(firstReadToken);

        RenderGraph secondGraph;
        RenderGraphValidationAccess::SetDevice(secondGraph, &device);
        const std::optional<GPUSceneResidentGraphLease> secondLease =
            uploader.AcquireCurrentGraphLease(secondGraph, nullptr, version);
        ASSERT_TRUE(secondLease.has_value());
        RenderGraph::RecordedQueueSubmission secondRecorded;
        recordGPUSceneFrame(secondGraph, *secondLease, secondRecorded);

        const RenderGraph::SubmissionPlan plan = secondGraph.GetSubmissionPlan();
        const auto releaseBatch = std::find_if(
            plan.queueBatches.begin(),
            plan.queueBatches.end(),
            [](const RenderGraph::PlannedQueueBatchDiagnostic& batch)
            {
                return batch.syntheticInitialRelease &&
                       batch.queue ==
                           RenderGraph::DiagnosticExecutionQueue::Graphics;
            });
        ASSERT_NE(releaseBatch, plan.queueBatches.end());
        ASSERT_LT(releaseBatch->batchIndex,
                  secondRecorded.ownedContexts.size());
        const auto* releaseContext = static_cast<const FakeCommandContext*>(
            secondRecorded.ownedContexts[releaseBatch->batchIndex].Get());
        ASSERT_NE(releaseContext, nullptr);

        for (uint32 tableIndex = 0;
             tableIndex < GPU_SCENE_RESIDENT_TABLE_COUNT;
             ++tableIndex)
        {
            const bool releasedFromGraphics = std::any_of(
                releaseContext->bufferBarriers.begin(),
                releaseContext->bufferBarriers.end(),
                [&, tableIndex](const RHIBufferBarrier& barrier)
                {
                    return barrier.buffer ==
                               secondLease->buffers[tableIndex].Get() &&
                           barrier.accessBefore.domain ==
                               GPUQueueDomain::Graphics &&
                           barrier.accessAfter.domain ==
                               GPUQueueDomain::Compute &&
                           HasDependencyKind(
                               barrier.dependencyKind,
                               RHIDependencyKind::Ownership);
                });
            const bool rasterTable =
                tableIndex == static_cast<uint32>(
                                  GPUSceneResidentTable::Primitives) ||
                tableIndex == static_cast<uint32>(
                                  GPUSceneResidentTable::Transforms);
            EXPECT_EQ(releasedFromGraphics, rasterTable) << tableIndex;
        }
        uploader.ReleaseUnsubmittedFrame();
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
        RenderGraphValidationAccess::SetDevice(unusedGraph, &device);
        ASSERT_TRUE(uploader.AcquireCurrentGraphLease(
            unusedGraph, nullptr, database.GetCommittedVersion()).has_value());
        EXPECT_TRUE(uploader.CancelCurrentGraphLease());
        EXPECT_FALSE(uploader.CancelCurrentGraphLease());

        RenderGraph retryGraph;
        RenderGraphValidationAccess::SetDevice(retryGraph, &device);
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

        // This test records with the GraphicsOnly adapter. Match that policy
        // in its fake capability snapshot rather than importing Copy-owned
        // staging into a graph that has no submission plan.
        const bool queueSubmissionPlanSupported =
            device.GetCapabilities().supportsQueueSubmissionPlan;
        device.SetQueueSubmissionPlanSupported(false);
        RenderGraph pendingGraph;
        RenderGraphValidationAccess::SetDevice(pendingGraph, &device);
        uploader.BuildRenderGraph(pendingGraph, nullptr);
        EXPECT_FALSE(uploader.AcquireCurrentGraphLease(
            pendingGraph, nullptr, database.GetCommittedVersion()).has_value());

        FakeCommandContext context;
        RenderGraphValidationAccess::Compile(pendingGraph);
        ASSERT_TRUE(pendingGraph.GetCompileStats().compileValid);
        RenderGraphValidationAccess::Execute(pendingGraph, context);
        uploader.CommitRealizedAccess(pendingGraph);
        device.SetQueueSubmissionPlanSupported(queueSubmissionPlanSupported);
        const GPUCompletionPoint uploadPoint = tracker.Submit(&context);
        GPUCompletionToken uploadToken;
        ASSERT_TRUE(InsertGPUCompletionPoint(uploadToken, uploadPoint));
        uploader.NotifySubmission(uploadToken);

        RenderGraph residentGraph;
        RenderGraphValidationAccess::SetDevice(residentGraph, &device);
        ASSERT_TRUE(uploader.AcquireCurrentGraphLease(
            residentGraph, nullptr, database.GetCommittedVersion()).has_value());
        uploader.ReleaseUnsubmittedFrame();

        GPUSceneTransaction update;
        update.Update(database.FindPrimitive(1).value(), MakeObject(1, 2.0F));
        ASSERT_TRUE(database.Commit(update).Succeeded());
        uploader.Observe(database.GetCommittedMirror(), database.GetLastChangeSet());
        RenderGraph mismatchGraph;
        RenderGraphValidationAccess::SetDevice(mismatchGraph, &device);
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
        RenderGraphValidationAccess::SetDevice(graph, &device);
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
        RenderGraphValidationAccess::SetDevice(rejectedGraph, &device);
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
        RenderGraphValidationAccess::SetDevice(versionTwoGraph, &device);
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
        RenderGraphValidationAccess::SetDevice(afterInvalid, &device);
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
        RenderGraphValidationAccess::SetDevice(omittedGraph, &device);
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
        RenderGraphValidationAccess::SetDevice(afterOmittedCommit, &device);
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
        RenderGraphValidationAccess::SetDevice(graph, &device);
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
        EXPECT_EQ(uploader.GetDiagnostics().frameUploadBytes, 0U);
        EXPECT_EQ(uploader.GetDiagnostics().frameUploadRangeCount, 0U);
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
        EXPECT_FALSE(uploader.GetDiagnostics().fullUpload);
        EXPECT_GE(device.DefaultBufferCount(), beforeGrowth + 6U);
        for (const char* name : {"GPUScene.Primitives", "GPUScene.Bounds", "GPUScene.Transforms",
                                 "GPUScene.Materials", "GPUScene.Geometries", "GPUScene.Draws"})
        {
            FakeBuffer* table = device.Find(name);
            ASSERT_NE(table, nullptr);
            EXPECT_EQ(0U, table->MapCalls());
        }
        const GPUCompletionPoint fullRetry = tracker.Submit(&context);
        GPUCompletionToken fullRetryToken;
        ASSERT_TRUE(InsertGPUCompletionPoint(fullRetryToken, fullRetry));
        uploader.NotifySubmission(fullRetryToken);
        EXPECT_TRUE(uploader.GetDiagnostics().fullUpload);
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
        const GPUCompletionPoint readPoint = tracker.Submit(&graphics);
        GPUCompletionToken readToken;
        ASSERT_TRUE(InsertGPUCompletionPoint(readToken, readPoint));
        uploader.NotifySubmission(readToken);

        device.Fence(0)->Complete(token.points[0].value);
        EXPECT_EQ(uploader.PollSafeReclaimVersion(), 0U);
        const uint32 before = device.DefaultBufferCount();
        GPUSceneTransaction update;
        update.Update(database.FindPrimitive(1).value(), MakeObject(1, 5.0F));
        ASSERT_TRUE(database.Commit(update).Succeeded());
        uploader.Observe(database.GetCommittedMirror(), database.GetLastChangeSet());
        RenderGraph graph;
        RenderGraphValidationAccess::SetDevice(graph, &device);
        uploader.BuildRenderGraph(graph, nullptr);
        EXPECT_GE(device.DefaultBufferCount(), before + 6U);
        uploader.ReleaseUnsubmittedFrame();

        device.Fence(1)->Complete(token.points[1].value);
        EXPECT_EQ(uploader.PollSafeReclaimVersion(), 0U);
        device.Fence(2)->Complete(token.points[2].value);
        EXPECT_EQ(uploader.PollSafeReclaimVersion(), 0U);
        device.Fence(0)->Complete(readPoint.value);
        EXPECT_GE(uploader.PollSafeReclaimVersion(), database.GetCommittedVersion() - 1U);
    }

    TEST(GPUSceneUploadValidation,
         StagingCommitFailureDoesNotPublishPartialTransactionAndCanRetry)
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
        const uint64 version = database.GetCommittedVersion();
        uploader.Observe(database.GetCommittedMirror(), database.GetLastChangeSet());

        // The first staging write commits, the second fails. The uploader must
        // not record a copy pass, consume dirty state, or claim either a prior
        // or requested resident version while the multi-table transaction is
        // incomplete.
        device.FailUploadCommitAfter(1);
        RenderGraph failedGraph;
        RenderGraphValidationAccess::SetDevice(failedGraph, &device);
        uploader.BuildRenderGraph(failedGraph, nullptr);
        const GPUSceneUploadDiagnostics& failed = uploader.GetDiagnostics();
        EXPECT_EQ(failed.failureReason,
                  GPUSceneUploadFailureReason::StagingCommitFailed);
        EXPECT_EQ(failed.frameUploadBytes, 0U);
        EXPECT_EQ(failed.cumulativeUploadBytes, 0U);
        EXPECT_EQ(failed.frameUploadRangeCount, 0U);
        EXPECT_EQ(failed.cumulativeUploadRangeCount, 0U);
        EXPECT_EQ(failed.uploadWork.gpuCopyBytes, 0U);
        EXPECT_EQ(failed.uploadWork.gpuCopyRangeCount, 0U);
        EXPECT_EQ(failed.uploadWork.mappedRangeCount, 2U);
        EXPECT_EQ(failed.uploadWork.committedRangeCount, 1U);
        EXPECT_GT(failed.uploadWork.cpuCopiedPayloadBytes, 0U);
        EXPECT_GT(failed.uploadWork.committedPayloadBytes, 0U);
        EXPECT_FALSE(failed.uploadWork.hostVisibilitySynchronizedBytes.IsAvailable());
        EXPECT_EQ(failed.tables[static_cast<uint32>(
                      GPUSceneDiagnosticsTable::Primitives)]
                      .uploadWork.committedRangeCount,
                  1U);
        EXPECT_EQ(failed.tables[static_cast<uint32>(
                      GPUSceneDiagnosticsTable::Bounds)]
                      .uploadWork.mappedRangeCount,
                  1U);
        EXPECT_EQ(failed.tables[static_cast<uint32>(
                      GPUSceneDiagnosticsTable::Bounds)]
                      .uploadWork.committedRangeCount,
                  0U);
        EXPECT_FALSE(failed.fullUpload);
        EXPECT_EQ(device.DefaultBufferCount(), 6U);
        EXPECT_EQ(device.UploadCommitCalls(), 2U);
        EXPECT_FALSE(uploader.QueryExactVersionReadiness(version).IsReady());

        FakeCommandContext context;
        RenderGraphValidationAccess::Compile(failedGraph);
        ASSERT_TRUE(failedGraph.GetCompileStats().compileValid);
        RenderGraphValidationAccess::Execute(failedGraph, context);
        EXPECT_EQ(context.copyCount, 0U);

        RenderGraph leaseGraph;
        RenderGraphValidationAccess::SetDevice(leaseGraph, &device);
        EXPECT_FALSE(uploader.AcquireCurrentGraphLease(leaseGraph, nullptr, version));

        // Commit failure applies to a single staging allocation. Retrying must
        // re-stage every still-dirty table into the existing resident set.
        RecordAndExecute(uploader, device, context);
        EXPECT_GT(context.copyCount, 0U);
        const GPUSceneUploadDiagnostics& recorded = uploader.GetDiagnostics();
        EXPECT_GT(recorded.uploadWork.committedPayloadBytes, 0U);
        EXPECT_EQ(recorded.uploadWork.gpuCopyBytes, 0U);
        EXPECT_EQ(recorded.uploadWork.gpuCopyRangeCount, 0U);
        EXPECT_EQ(recorded.mutationTotals.submittedUploadCount, 0U);
        const GPUCompletionPoint point = tracker.Submit(&context);
        ASSERT_NE(point.value, 0U);
        GPUCompletionToken token;
        ASSERT_TRUE(InsertGPUCompletionPoint(token, point));
        uploader.NotifySubmission(token);
        ASSERT_NE(device.Fence(0), nullptr);
        device.Fence(0)->Complete(point.value);

        const GPUSceneUploadDiagnostics& retried = uploader.GetDiagnostics();
        EXPECT_EQ(retried.failureReason, GPUSceneUploadFailureReason::None);
        EXPECT_GT(retried.cumulativeUploadBytes, 0U);
        EXPECT_GT(retried.cumulativeUploadRangeCount, 0U);
        EXPECT_GT(retried.uploadWork.gpuCopyBytes, 0U);
        EXPECT_GT(retried.uploadWork.gpuCopyRangeCount, 0U);
        EXPECT_TRUE(uploader.QueryExactVersionReadiness(version).IsReady());
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
        RenderGraphValidationAccess::SetDevice(graph, &device);
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
        RenderGraphValidationAccess::SetDevice(stagingGraph, &stagingDevice);
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

    TEST(GPUSceneUploadValidation,
         RecordedUploadRejectsPreRecordingTokenAndPublishesCopyTelemetryOnlyOnce)
    {
        FakeDevice device;
        RenderSubmissionTracker tracker;
        ASSERT_TRUE(tracker.Initialize(&device));
        GPUSceneUploader uploader;
        ASSERT_TRUE(uploader.Initialize(&device, &tracker));
        GPUSceneDatabase database;
        GPUSceneTransaction add;
        add.Add(MakeObject(77, 7.0F));
        ASSERT_TRUE(database.Commit(add).Succeeded());
        uploader.Observe(database.GetCommittedMirror(), database.GetLastChangeSet());

        FakeCommandContext context;
        const GPUCompletionPoint oldPoint = tracker.Submit(&context);
        ASSERT_NE(oldPoint.value, 0U);
        GPUCompletionToken oldToken;
        ASSERT_TRUE(InsertGPUCompletionPoint(oldToken, oldPoint));

        RecordAndExecute(uploader, device, context);
        const GPUSceneUploadDiagnostics& recorded = uploader.GetDiagnostics();
        EXPECT_GT(recorded.uploadWork.committedPayloadBytes, 0U);
        EXPECT_EQ(recorded.frameUploadBytes, 0U);
        EXPECT_EQ(recorded.cumulativeUploadBytes, 0U);
        EXPECT_EQ(recorded.uploadWork.gpuCopyBytes, 0U);
        EXPECT_EQ(recorded.uploadWork.gpuCopyRangeCount, 0U);

        // The prior point was current when recording began, but it cannot own
        // this later graph recording or publish its residency/copy evidence.
        uploader.NotifySubmission(oldToken);
        const GPUSceneUploadDiagnostics& rejected = uploader.GetDiagnostics();
        EXPECT_EQ(rejected.failureReason,
                  GPUSceneUploadFailureReason::InvalidCompletionToken);
        EXPECT_EQ(rejected.residentVersion, 0U);
        EXPECT_EQ(rejected.frameUploadBytes, 0U);
        EXPECT_EQ(rejected.cumulativeUploadBytes, 0U);
        EXPECT_EQ(rejected.frameUploadRangeCount, 0U);
        EXPECT_EQ(rejected.cumulativeUploadRangeCount, 0U);
        EXPECT_EQ(rejected.uploadWork.gpuCopyBytes, 0U);
        EXPECT_EQ(rejected.uploadWork.gpuCopyRangeCount, 0U);
        EXPECT_EQ(rejected.mutationTotals.submittedUploadCount, 0U);
        EXPECT_EQ(rejected.mutationTotals.uploadBytes, 0U);
        for (const uint64 rows :
             rejected.mutationTotals.submittedUploadedRowCount)
        {
            EXPECT_EQ(rows, 0U);
        }
        EXPECT_GT(rejected.uploadWork.committedPayloadBytes, 0U);
        const uint64 rejectedHostBytes =
            rejected.uploadWork.committedPayloadBytes;
        for (const GPUSceneTableDiagnostics& table : rejected.tables)
        {
            EXPECT_EQ(table.frameUploadBytes, 0U);
            EXPECT_EQ(table.cumulativeUploadBytes, 0U);
            EXPECT_EQ(table.uploadWork.gpuCopyBytes, 0U);
            EXPECT_EQ(table.uploadWork.gpuCopyRangeCount, 0U);
        }

        // A later scheduler query must preserve that retained attempt rather
        // than replacing the published host receipts with a zero-work frame.
        RenderGraph retryGraph;
        RenderGraphValidationAccess::SetDevice(retryGraph, &device);
        uploader.BuildRenderGraph(retryGraph, nullptr);
        EXPECT_EQ(uploader.GetDiagnostics().uploadWork.committedPayloadBytes,
                  rejectedHostBytes);

        // The recording that reached the command context is retained.  P1
        // owns those exact commands; a second graph build would be neither
        // necessary nor permitted while the pending plan is awaiting proof.
        const GPUCompletionPoint currentPoint = tracker.Submit(&context);
        ASSERT_NE(currentPoint.value, 0U);
        GPUCompletionToken currentToken;
        ASSERT_TRUE(InsertGPUCompletionPoint(currentToken, currentPoint));
        uploader.NotifySubmission(currentToken);
        const GPUSceneUploadDiagnostics& published = uploader.GetDiagnostics();
        EXPECT_EQ(published.failureReason, GPUSceneUploadFailureReason::None);
        EXPECT_EQ(published.residentVersion, database.GetCommittedVersion());
        EXPECT_GT(published.frameUploadBytes, 0U);
        EXPECT_EQ(published.frameUploadBytes, published.cumulativeUploadBytes);
        EXPECT_GT(published.frameUploadRangeCount, 0U);
        EXPECT_EQ(published.frameUploadRangeCount,
                  published.cumulativeUploadRangeCount);
        EXPECT_EQ(published.uploadWork.gpuCopyBytes,
                  published.frameUploadBytes);
        EXPECT_EQ(published.uploadWork.gpuCopyRangeCount,
                  published.frameUploadRangeCount);
        EXPECT_EQ(published.mutationTotals.submittedUploadCount, 1U);
        EXPECT_EQ(published.mutationTotals.uploadBytes,
                  published.frameUploadBytes);
        EXPECT_EQ(published.mutationTotals.uploadRangeCount,
                  published.frameUploadRangeCount);
        EXPECT_EQ(published.mutationTotals.fullUploadCount, 1U);
        for (uint32 tableIndex = 0;
             tableIndex < GPU_SCENE_DIAGNOSTICS_TABLE_COUNT;
             ++tableIndex)
        {
            EXPECT_EQ(published.mutationTotals
                          .submittedUploadedRowCount[tableIndex],
                      published.tables[tableIndex].frameUploadBytes /
                          published.tables[tableIndex].stride);
        }

        const uint64 cumulativeBytes = published.cumulativeUploadBytes;
        const uint64 cumulativeRanges = published.cumulativeUploadRangeCount;
        uploader.NotifySubmission(currentToken);
        EXPECT_EQ(uploader.GetDiagnostics().cumulativeUploadBytes, cumulativeBytes);
        EXPECT_EQ(uploader.GetDiagnostics().cumulativeUploadRangeCount,
                  cumulativeRanges);
        EXPECT_EQ(uploader.GetDiagnostics().mutationTotals.submittedUploadCount,
                  1U);
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
        RenderGraphValidationAccess::SetDevice(graph, &device);
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
        EXPECT_EQ(recorded.frameUploadBytes, 0U);
        EXPECT_EQ(recorded.cumulativeUploadBytes, 0U);
        EXPECT_EQ(recorded.frameUploadRangeCount, 0U);
        EXPECT_FALSE(recorded.fullUpload);
        EXPECT_GT(recorded.uploadWork.committedPayloadBytes, 0U);
        EXPECT_GT(recorded.gpuAllocationBytes, 0U);
        EXPECT_EQ(recorded.bufferSetCount, 1U);
        for (const GPUSceneTableDiagnostics& table : recorded.tables)
        {
            EXPECT_EQ(table.frameUploadBytes, 0U);
            EXPECT_FALSE(table.fullUpload);
        }

        const GPUCompletionPoint submitted = tracker.Submit(&context);
        GPUCompletionToken token;
        ASSERT_TRUE(InsertGPUCompletionPoint(token, submitted));
        uploader.NotifySubmission(token);
        const GPUSceneUploadDiagnostics committed = uploader.GetDiagnostics();
        EXPECT_GT(committed.frameUploadBytes, 0U);
        EXPECT_EQ(committed.frameUploadBytes, committed.cumulativeUploadBytes);
        EXPECT_EQ(committed.frameUploadBytes, committed.peakFrameUploadBytes);
        EXPECT_TRUE(committed.fullUpload);
        for (const GPUSceneTableDiagnostics& table : committed.tables)
        {
            EXPECT_GT(table.frameUploadBytes, 0U);
            EXPECT_TRUE(table.fullUpload);
        }
        CompleteToken(device, token);
        ASSERT_GE(uploader.PollSafeReclaimVersion(), database.GetCommittedVersion());

        RenderGraph warmGraph;
        RenderGraphValidationAccess::SetDevice(warmGraph, &device);
        uploader.BuildRenderGraph(warmGraph, nullptr);
        RenderGraphValidationAccess::Compile(warmGraph);
        ASSERT_TRUE(warmGraph.GetCompileStats().compileValid);
        const GPUSceneUploadDiagnostics warm = uploader.GetDiagnostics();
        EXPECT_EQ(warm.frameUploadBytes, 0U);
        EXPECT_EQ(warm.frameUploadRangeCount, 0U);
        EXPECT_EQ(warm.cumulativeUploadBytes, committed.cumulativeUploadBytes);
        EXPECT_GE(warm.peakFrameUploadBytes, committed.peakFrameUploadBytes);
        EXPECT_EQ(warm.tables[static_cast<uint32>(GPUSceneDiagnosticsTable::Primitives)].
                      residentCapacity,
                  2U);
        EXPECT_FALSE(warm.executionEligible);
        const GPUSceneUploadMutationTotals beforeIdentityOnlyNotify =
            warm.mutationTotals;
        EXPECT_TRUE(uploader.NotifySubmission({}));
        const GPUSceneUploadMutationTotals afterIdentityOnlyNotify =
            uploader.GetDiagnostics().mutationTotals;
        EXPECT_EQ(afterIdentityOnlyNotify.submittedUploadCount,
                  beforeIdentityOnlyNotify.submittedUploadCount);
        EXPECT_EQ(afterIdentityOnlyNotify.uploadBytes,
                  beforeIdentityOnlyNotify.uploadBytes);
        EXPECT_EQ(afterIdentityOnlyNotify.uploadRangeCount,
                  beforeIdentityOnlyNotify.uploadRangeCount);
        EXPECT_EQ(afterIdentityOnlyNotify.submittedUploadedRowCount,
                  beforeIdentityOnlyNotify.submittedUploadedRowCount);
    }
} // namespace
