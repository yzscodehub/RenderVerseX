#include "Core/Log.h"
#include "Render/Graph/ResourceViewCache.h"
#include "Render/Graph/TransientResourcePool.h"
#include "Resources/RenderOwnerSnapshotRetirement.h"
#include "Resources/RenderRetirementQueue.h"
#include "Resources/RenderSubmissionResourceBatch.h"
#include "Resources/RenderSubmissionTracker.h"
#include "RHI/RHICapabilities.h"
#include "RHI/RHICommandContext.h"
#include "RHI/RHIDevice.h"

#include <gtest/gtest.h>

#include <array>
#include <memory>
#include <span>
#include <thread>
#include <vector>

namespace RVX::Tests
{
    namespace
    {
        class LogEnvironment final : public ::testing::Environment
        {
        public:
            void SetUp() override { Log::Initialize(); }
            void TearDown() override { Log::Shutdown(); }
        };

        [[maybe_unused]] ::testing::Environment* const g_logEnvironment =
            ::testing::AddGlobalTestEnvironment(new LogEnvironment());

        class FakeCommandContext final : public RHICommandContext
        {
        public:
            explicit FakeCommandContext(RHICommandQueueType queueType)
                : m_queueType(queueType)
            {
            }

            RHICommandQueueType GetQueueType() const override { return m_queueType; }
            void Begin() override {}
            void End() override {}
            void Reset() override {}
            void BeginEvent(const char*, uint32 = 0) override {}
            void EndEvent() override {}
            void SetMarker(const char*, uint32 = 0) override {}
            void BufferBarrier(const RHIBufferBarrier&) override {}
            void TextureBarrier(const RHITextureBarrier&) override {}
            void Barriers(std::span<const RHIBufferBarrier>,
                          std::span<const RHITextureBarrier>) override {}
            void BeginBarrier(const RHIBufferBarrier&) override {}
            void BeginBarrier(const RHITextureBarrier&) override {}
            void EndBarrier(const RHIBufferBarrier&) override {}
            void EndBarrier(const RHITextureBarrier&) override {}
            void BeginRenderPass(const RHIRenderPassDesc&) override {}
            void EndRenderPass() override {}
            void SetPipeline(RHIPipeline*) override {}
            void SetVertexBuffer(uint32, RHIBuffer*, uint64 = 0) override {}
            void SetVertexBuffers(uint32, std::span<RHIBuffer* const>,
                                  std::span<const uint64> = {}) override {}
            void SetIndexBuffer(RHIBuffer*, RHIFormat, uint64 = 0) override {}
            void SetDescriptorSet(uint32, RHIDescriptorSet*,
                                  std::span<const uint32> = {}) override {}
            void SetPushConstants(const void*, uint32, uint32 = 0) override {}
            void SetViewport(const RHIViewport&) override {}
            void SetViewports(std::span<const RHIViewport>) override {}
            void SetScissor(const RHIRect&) override {}
            void SetScissors(std::span<const RHIRect>) override {}
            void Draw(uint32, uint32 = 1, uint32 = 0, uint32 = 0) override {}
            void DrawIndexed(uint32, uint32 = 1, uint32 = 0,
                             int32 = 0, uint32 = 0) override {}
            void DrawIndirect(RHIBuffer*, uint64, uint32, uint32) override {}
            void DrawIndexedIndirect(RHIBuffer*, uint64, uint32, uint32) override {}
            void Dispatch(uint32, uint32, uint32) override {}
            void DispatchIndirect(RHIBuffer*, uint64) override {}
            void CopyBuffer(RHIBuffer*, RHIBuffer*, uint64, uint64, uint64) override {}
            void CopyTexture(RHITexture*, RHITexture*,
                             const RHITextureCopyDesc& = {}) override {}
            void CopyBufferToTexture(RHIBuffer*, RHITexture*,
                                     const RHIBufferTextureCopyDesc&) override {}
            void CopyTextureToBuffer(RHITexture*, RHIBuffer*,
                                     const RHIBufferTextureCopyDesc&) override {}
            void BeginQuery(RHIQueryPool*, uint32) override {}
            void EndQuery(RHIQueryPool*, uint32) override {}
            void WriteTimestamp(RHIQueryPool*, uint32) override {}
            void ResolveQueries(RHIQueryPool*, uint32, uint32,
                                RHIBuffer*, uint64) override {}
            void ResetQueries(RHIQueryPool*, uint32, uint32) override {}
            void SetStencilReference(uint32) override {}
            void SetBlendConstants(const float[4]) override {}
            void SetDepthBias(float, float, float = 0.0f) override {}
            void SetDepthBounds(float, float) override {}
            void SetStencilReferenceSeparate(uint32, uint32) override {}
            void SetLineWidth(float) override {}
            void SignalFence(RHIFence*, uint64) override {}
            void WaitFence(RHIFence*, uint64) override {}

        private:
            RHICommandQueueType m_queueType = RHICommandQueueType::Graphics;
        };

        class FakeFence final : public RHIFence
        {
        public:
            explicit FakeFence(uint64 initialValue)
                : m_completedValue(initialValue)
                , m_nextValue(initialValue + 1)
            {
            }

            uint64 GetCompletedValue() const override { return m_completedValue; }
            void Signal(uint64 value) override { Complete(value); }
            void SignalOnQueue(uint64 value, RHICommandQueueType) override { Complete(value); }
            void Wait(uint64 value, uint64 = UINT64_MAX) override { Complete(value); }
            uint64 AllocateValue() { return m_nextValue++; }
            void Complete(uint64 value) { m_completedValue = std::max(m_completedValue, value); }
            void MarkLost() { m_completedValue = UINT64_MAX; }

        private:
            uint64 m_completedValue = 0;
            uint64 m_nextValue = 1;
        };

        struct ResourceProbeState
        {
            bool destroyed = false;
            std::thread::id destructionThread{};
        };

        class FakeTexture final : public RHITexture
        {
        public:
            FakeTexture(RHITextureDesc desc,
                        std::shared_ptr<ResourceProbeState> state)
                : m_desc(std::move(desc))
                , m_state(std::move(state))
            {
            }

            ~FakeTexture() override
            {
                m_state->destroyed = true;
                m_state->destructionThread = std::this_thread::get_id();
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
            std::shared_ptr<ResourceProbeState> m_state;
        };

        class FakeTextureView final : public RHITextureView
        {
        public:
            FakeTextureView(RHITexture* texture,
                            RHITextureViewDesc desc,
                            std::shared_ptr<ResourceProbeState> state)
                : RHITextureView(RHITextureRef(texture))
                , m_desc(std::move(desc))
                , m_state(std::move(state))
            {
            }

            ~FakeTextureView() override
            {
                m_state->destroyed = true;
                m_state->destructionThread = std::this_thread::get_id();
            }

            RHIFormat GetFormat() const override { return m_desc.format; }
            const RHISubresourceRange& GetSubresourceRange() const override
            {
                return m_desc.subresourceRange;
            }

        private:
            RHITextureViewDesc m_desc;
            std::shared_ptr<ResourceProbeState> m_state;
        };

        RHICapabilities MakeCapabilities(RHIQueueCompletionMode mode)
        {
            RHICapabilities capabilities;
            capabilities.backendType =
                mode == RHIQueueCompletionMode::CompatibilityWaitIdle
                    ? RHIBackendType::DX11
                    : RHIBackendType::DX12;
            capabilities.adapterName = "LifetimeFake";
            capabilities.driverVersion = "1";
            capabilities.supportsComputePipeline = true;
            capabilities.supportsDescriptorSets = true;
            capabilities.supportsDynamicDescriptorOffsets = true;
            capabilities.maxDescriptorSets = 4;
            capabilities.supportsExplicitResourceBarriers = true;
            capabilities.supportsDefaultQueueFenceSignal =
                mode == RHIQueueCompletionMode::NativeTimeline;
            capabilities.supportsExplicitQueueFenceSignal =
                mode == RHIQueueCompletionMode::NativeTimeline;
            capabilities.emulatesQueueFences =
                mode == RHIQueueCompletionMode::CompatibilityWaitIdle;
            capabilities.supportsAsyncCompute =
                mode == RHIQueueCompletionMode::NativeTimeline;
            capabilities.dx12.resourceBindingTier = 2;
            capabilities.queueTopology.completionMode = mode;
            capabilities.queueTopology.logicalQueueDomains =
                mode == RHIQueueCompletionMode::NativeTimeline
                    ? std::array<GPUQueueDomain, 3>{
                          GPUQueueDomain::Graphics,
                          GPUQueueDomain::Compute,
                          GPUQueueDomain::Copy}
                    : std::array<GPUQueueDomain, 3>{
                          GPUQueueDomain::Graphics,
                          GPUQueueDomain::Graphics,
                          GPUQueueDomain::Graphics};
            capabilities.queueTopology.activeDomainCount =
                mode == RHIQueueCompletionMode::NativeTimeline ? 3 : 1;
            return capabilities;
        }

        class FakeDevice final : public IRHIDevice
        {
        public:
            explicit FakeDevice(RHIQueueCompletionMode mode)
                : m_capabilities(MakeCapabilities(mode))
            {
            }

            RHIBufferRef CreateBuffer(const RHIBufferDesc&) override { return {}; }
            RHITextureRef CreateTexture(const RHITextureDesc& desc) override
            {
                auto state = std::make_shared<ResourceProbeState>();
                textureStates.push_back(state);
                return RHITextureRef(new FakeTexture(desc, std::move(state)));
            }
            RHITextureViewRef CreateTextureView(
                RHITexture* texture,
                const RHITextureViewDesc& desc = {}) override
            {
                auto state = std::make_shared<ResourceProbeState>();
                viewStates.push_back(state);
                return RHITextureViewRef(
                    new FakeTextureView(texture, desc, std::move(state)));
            }
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
            RHICommandContextRef CreateCommandContext(RHICommandQueueType type) override
            {
                return RHICommandContextRef(new FakeCommandContext(type));
            }
            uint64 SubmitCommandContext(RHICommandContext*, RHIFence* signalFence) override
            {
                if (failNextSubmit || !signalFence)
                {
                    failNextSubmit = false;
                    return 0;
                }
                return static_cast<FakeFence*>(signalFence)->AllocateValue();
            }
            uint64 SubmitCommandContexts(std::span<RHICommandContext* const>, RHIFence*) override { return 0; }
            RHISwapChainRef CreateSwapChain(const RHISwapChainDesc&) override { return {}; }
            RHIFenceRef CreateFence(uint64 initialValue = 0) override
            {
                RHIFenceRef fence(new FakeFence(initialValue));
                fences.push_back(fence);
                return fence;
            }
            void WaitForFence(RHIFence* fence, uint64 value) override { fence->Wait(value); }
            void WaitIdle() override { ++waitIdleCount; }
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
                return m_capabilities.backendType;
            }

            FakeFence* Fence(size_t index) const
            {
                return static_cast<FakeFence*>(fences[index].Get());
            }

            RHICapabilities m_capabilities;
            std::vector<RHIFenceRef> fences;
            std::vector<std::shared_ptr<ResourceProbeState>> textureStates;
            std::vector<std::shared_ptr<ResourceProbeState>> viewStates;
            uint32 waitIdleCount = 0;
            bool failNextSubmit = false;
        };

        struct ProbeState
        {
            bool destroyed = false;
            std::thread::id destructionThread{};
        };

        class Probe final : public RefCounted
        {
        public:
            explicit Probe(std::shared_ptr<ProbeState> state)
                : m_state(std::move(state))
            {
            }

            ~Probe() override
            {
                m_state->destroyed = true;
                m_state->destructionThread = std::this_thread::get_id();
            }

        private:
            std::shared_ptr<ProbeState> m_state;
        };

        Ref<RefCounted> MakeProbe(const std::shared_ptr<ProbeState>& state)
        {
            return Ref<RefCounted>(new Probe(state));
        }
    } // namespace

    TEST(RenderLifetimeCutoverValidation, LifetimePolicyValuesAreStable)
    {
        static_assert(static_cast<uint8>(RenderRHILifetimePolicy::RegistryExactGeneration) == 0);
        static_assert(static_cast<uint8>(RenderRHILifetimePolicy::SubmissionBatch) == 1);
        static_assert(static_cast<uint8>(RenderRHILifetimePolicy::PoolAvailability) == 2);
        static_assert(static_cast<uint8>(RenderRHILifetimePolicy::OwnerSnapshot) == 3);
        static_assert(static_cast<uint8>(RenderRHILifetimePolicy::SurfaceGeneration) == 4);
        static_assert(static_cast<uint8>(RenderRHILifetimePolicy::ShutdownAfterDrain) == 5);
    }

    TEST(RenderLifetimeCutoverValidation, SubmissionBatchDeduplicatesAndWaitsForEveryDomain)
    {
        FakeDevice device(RHIQueueCompletionMode::NativeTimeline);
        RenderSubmissionTracker tracker;
        ASSERT_TRUE(tracker.Initialize(&device));
        RenderRetirementQueue retirement;
        ASSERT_TRUE(retirement.Initialize(&tracker));

        FakeCommandContext graphics(RHICommandQueueType::Graphics);
        FakeCommandContext copy(RHICommandQueueType::Copy);
        GPUCompletionToken completion;
        const GPUCompletionPoint graphicsPoint = tracker.Submit(&graphics);
        const GPUCompletionPoint copyPoint = tracker.Submit(&copy);
        ASSERT_TRUE(InsertGPUCompletionPoint(completion, graphicsPoint));
        ASSERT_TRUE(InsertGPUCompletionPoint(completion, copyPoint));

        const auto state = std::make_shared<ProbeState>();
        Ref<RefCounted> probe = MakeProbe(state);
        RenderSubmissionResourceBatch batch;
        ASSERT_FALSE(batch.Retain({}, 10));
        ASSERT_TRUE(batch.Retain(probe, 64));
        ASSERT_TRUE(batch.Retain(probe, 256));
        EXPECT_EQ(batch.GetRetainedObjectCount(), 1u);
        probe.Reset();

        batch.SealAndTransfer(completion, retirement);
        EXPECT_TRUE(batch.IsSealed());
        EXPECT_EQ(batch.GetRetainedObjectCount(), 0u);
        EXPECT_FALSE(batch.Retain(MakeProbe(std::make_shared<ProbeState>())));
        EXPECT_EQ(retirement.GetDiagnostics().entryCount, 1u);
        EXPECT_EQ(retirement.GetDiagnostics().estimatedBytes, 256u);

        device.Fence(0)->Complete(graphicsPoint.value);
        EXPECT_EQ(retirement.Poll(), GPUCompletionStatus::Pending);
        EXPECT_FALSE(state->destroyed);
        device.Fence(2)->Complete(copyPoint.value);
        EXPECT_EQ(retirement.Poll(), GPUCompletionStatus::Completed);
        EXPECT_TRUE(state->destroyed);
        EXPECT_EQ(state->destructionThread, std::this_thread::get_id());
    }

    TEST(RenderLifetimeCutoverValidation, NeverSubmittedBatchReleasesImmediatelyOnRenderThread)
    {
        FakeDevice device(RHIQueueCompletionMode::NativeTimeline);
        RenderSubmissionTracker tracker;
        ASSERT_TRUE(tracker.Initialize(&device));
        RenderRetirementQueue retirement;
        ASSERT_TRUE(retirement.Initialize(&tracker));

        const auto state = std::make_shared<ProbeState>();
        RenderSubmissionResourceBatch batch;
        ASSERT_TRUE(batch.Retain(MakeProbe(state)));
        batch.ReleaseUnsubmitted(retirement);

        EXPECT_TRUE(batch.IsSealed());
        EXPECT_TRUE(state->destroyed);
        EXPECT_EQ(state->destructionThread, std::this_thread::get_id());
        EXPECT_EQ(retirement.GetDiagnostics().entryCount, 0u);
    }

    TEST(RenderLifetimeCutoverValidation, PartialSubmissionUsesOnlyTheActualReturnedToken)
    {
        FakeDevice device(RHIQueueCompletionMode::NativeTimeline);
        RenderSubmissionTracker tracker;
        ASSERT_TRUE(tracker.Initialize(&device));
        RenderRetirementQueue retirement;
        ASSERT_TRUE(retirement.Initialize(&tracker));

        FakeCommandContext graphics(RHICommandQueueType::Graphics);
        FakeCommandContext copy(RHICommandQueueType::Copy);
        const GPUCompletionPoint graphicsPoint = tracker.Submit(&graphics);
        device.failNextSubmit = true;
        EXPECT_EQ(tracker.Submit(&copy).value, 0u);

        GPUCompletionToken actualCompletion;
        ASSERT_TRUE(InsertGPUCompletionPoint(actualCompletion, graphicsPoint));
        const auto state = std::make_shared<ProbeState>();
        RenderSubmissionResourceBatch batch;
        ASSERT_TRUE(batch.Retain(MakeProbe(state)));
        batch.SealAndTransfer(actualCompletion, retirement);

        EXPECT_EQ(retirement.Poll(), GPUCompletionStatus::Pending);
        EXPECT_FALSE(state->destroyed);
        device.Fence(0)->Complete(graphicsPoint.value);
        EXPECT_EQ(retirement.Poll(), GPUCompletionStatus::Completed);
        EXPECT_TRUE(state->destroyed);
    }

    TEST(RenderLifetimeCutoverValidation, BatchRejectsRecordingFromAnotherThread)
    {
        FakeDevice device(RHIQueueCompletionMode::NativeTimeline);
        RenderSubmissionTracker tracker;
        ASSERT_TRUE(tracker.Initialize(&device));
        RenderRetirementQueue retirement;
        ASSERT_TRUE(retirement.Initialize(&tracker));

        RenderSubmissionResourceBatch batch;
        Ref<RefCounted> probe = MakeProbe(std::make_shared<ProbeState>());
        bool retained = true;
        std::thread worker([&]() { retained = batch.Retain(probe); });
        worker.join();
        EXPECT_FALSE(retained);
        EXPECT_EQ(batch.GetRetainedObjectCount(), 0u);
        batch.ReleaseUnsubmitted(retirement);
    }

    TEST(RenderLifetimeCutoverValidation, DeviceLostTeardownReleasesSealedBatchExplicitly)
    {
        FakeDevice device(RHIQueueCompletionMode::NativeTimeline);
        RenderSubmissionTracker tracker;
        ASSERT_TRUE(tracker.Initialize(&device));
        RenderRetirementQueue retirement;
        ASSERT_TRUE(retirement.Initialize(&tracker));

        FakeCommandContext graphics(RHICommandQueueType::Graphics);
        GPUCompletionToken completion;
        ASSERT_TRUE(InsertGPUCompletionPoint(completion, tracker.Submit(&graphics)));
        const auto state = std::make_shared<ProbeState>();
        RenderSubmissionResourceBatch batch;
        ASSERT_TRUE(batch.Retain(MakeProbe(state)));
        batch.SealAndTransfer(completion, retirement);

        device.Fence(0)->MarkLost();
        EXPECT_EQ(retirement.Poll(), GPUCompletionStatus::Lost);
        EXPECT_FALSE(state->destroyed);
        EXPECT_EQ(retirement.ForceDeviceLostTeardown(), GPUCompletionStatus::Lost);
        EXPECT_TRUE(state->destroyed);
        EXPECT_EQ(state->destructionThread, std::this_thread::get_id());
    }

    TEST(RenderLifetimeCutoverValidation, OwnerReplacementWaitsForCapturedSubmissionSnapshot)
    {
        FakeDevice device(RHIQueueCompletionMode::NativeTimeline);
        RenderSubmissionTracker tracker;
        ASSERT_TRUE(tracker.Initialize(&device));
        RenderRetirementQueue retirement;
        ASSERT_TRUE(retirement.Initialize(&tracker));

        FakeCommandContext graphics(RHICommandQueueType::Graphics);
        const GPUCompletionPoint point = tracker.Submit(&graphics);
        GPUCompletionToken completion;
        ASSERT_TRUE(InsertGPUCompletionPoint(completion, point));

        const auto state = std::make_shared<ProbeState>();
        RenderOwnerRetirementList pending;
        Ref<RefCounted> owned = MakeProbe(state);
        QueueRenderOwnerRetirement(owned, pending);
        EXPECT_FALSE(owned);
        ASSERT_EQ(pending.size(), 1u);

        FlushRenderOwnerRetirements(pending, completion, retirement);
        EXPECT_TRUE(pending.empty());
        EXPECT_EQ(retirement.Poll(), GPUCompletionStatus::Pending);
        EXPECT_FALSE(state->destroyed);

        device.Fence(0)->Complete(point.value);
        EXPECT_EQ(retirement.Poll(), GPUCompletionStatus::Completed);
        EXPECT_TRUE(state->destroyed);
        EXPECT_EQ(state->destructionThread, std::this_thread::get_id());
    }

    TEST(RenderLifetimeCutoverValidation, PoolDoesNotReuseUntilAvailableAfterCompletes)
    {
        FakeDevice device(RHIQueueCompletionMode::NativeTimeline);
        RenderSubmissionTracker tracker;
        ASSERT_TRUE(tracker.Initialize(&device));
        RenderRetirementQueue retirement;
        ASSERT_TRUE(retirement.Initialize(&tracker));
        TransientResourcePool pool;
        pool.Initialize(&device, &tracker, &retirement);

        RHITextureDesc desc = RHITextureDesc::Texture2D(
            64, 64, RHIFormat::RGBA8_UNORM,
            RHITextureUsage::ShaderResource | RHITextureUsage::RenderTarget);
        pool.BeginFrame();
        RHITexture* first = pool.AcquireTexture(desc);
        ASSERT_NE(first, nullptr);

        FakeCommandContext graphics(RHICommandQueueType::Graphics);
        GPUCompletionToken completion;
        const GPUCompletionPoint point = tracker.Submit(&graphics);
        ASSERT_TRUE(InsertGPUCompletionPoint(completion, point));
        EXPECT_EQ(tracker.CaptureLastSubmittedToken().points[0], point);
        pool.NotifySubmission(completion);
        pool.ReleaseTexture(first);

        RHITexture* second = pool.AcquireTexture(desc);
        ASSERT_NE(second, nullptr);
        EXPECT_NE(second, first);
        EXPECT_EQ(device.textureStates.size(), 2u);
        pool.ReleaseTexture(second);

        device.Fence(0)->Complete(point.value);
        RHITexture* reused = pool.AcquireTexture(desc);
        EXPECT_TRUE(reused == first || reused == second);
        EXPECT_EQ(device.textureStates.size(), 2u);
        pool.ReleaseTexture(reused);

        pool.Shutdown();
        EXPECT_EQ(retirement.GetDiagnostics().entryCount, 2u);
        EXPECT_EQ(retirement.Poll(), GPUCompletionStatus::Completed);
        EXPECT_TRUE(device.textureStates[0]->destroyed);
        EXPECT_TRUE(device.textureStates[1]->destroyed);
    }

    TEST(RenderLifetimeCutoverValidation, PoolEvictionTransfersTheSamePendingToken)
    {
        FakeDevice device(RHIQueueCompletionMode::NativeTimeline);
        RenderSubmissionTracker tracker;
        ASSERT_TRUE(tracker.Initialize(&device));
        RenderRetirementQueue retirement;
        ASSERT_TRUE(retirement.Initialize(&tracker));
        TransientResourcePool pool;
        pool.Initialize(&device, &tracker, &retirement);

        const RHITextureDesc desc = RHITextureDesc::Texture2D(
            32, 32, RHIFormat::RGBA8_UNORM,
            RHITextureUsage::ShaderResource);
        pool.BeginFrame();
        RHITexture* texture = pool.AcquireTexture(desc);
        ASSERT_NE(texture, nullptr);
        FakeCommandContext graphics(RHICommandQueueType::Graphics);
        GPUCompletionToken completion;
        const GPUCompletionPoint point = tracker.Submit(&graphics);
        ASSERT_TRUE(InsertGPUCompletionPoint(completion, point));
        pool.NotifySubmission(completion);
        pool.ReleaseTexture(texture);
        pool.EvictUnused(0);

        ASSERT_EQ(device.textureStates.size(), 1u);
        EXPECT_FALSE(device.textureStates[0]->destroyed);
        EXPECT_EQ(retirement.GetDiagnostics().entryCount, 1u);
        EXPECT_EQ(retirement.Poll(), GPUCompletionStatus::Pending);
        device.Fence(0)->Complete(point.value);
        EXPECT_EQ(retirement.Poll(), GPUCompletionStatus::Completed);
        EXPECT_TRUE(device.textureStates[0]->destroyed);
        pool.Shutdown();
    }

    TEST(RenderLifetimeCutoverValidation, ViewInvalidationRetiresAfterItsLastUseToken)
    {
        FakeDevice device(RHIQueueCompletionMode::NativeTimeline);
        RenderSubmissionTracker tracker;
        ASSERT_TRUE(tracker.Initialize(&device));
        RenderRetirementQueue retirement;
        ASSERT_TRUE(retirement.Initialize(&tracker));
        ResourceViewCache cache;
        cache.Initialize(&device, &tracker, &retirement);

        const RHITextureDesc desc = RHITextureDesc::Texture2D(
            16, 16, RHIFormat::RGBA8_UNORM,
            RHITextureUsage::ShaderResource);
        RHITextureRef texture = device.CreateTexture(desc);
        ASSERT_NE(cache.GetDefaultSRV(texture.Get()), nullptr);
        FakeCommandContext graphics(RHICommandQueueType::Graphics);
        GPUCompletionToken completion;
        const GPUCompletionPoint point = tracker.Submit(&graphics);
        ASSERT_TRUE(InsertGPUCompletionPoint(completion, point));
        cache.NotifySubmission(completion);
        cache.InvalidateTexture(texture.Get());

        ASSERT_EQ(device.viewStates.size(), 1u);
        EXPECT_FALSE(device.viewStates[0]->destroyed);
        EXPECT_EQ(retirement.GetDiagnostics().entryCount, 1u);
        EXPECT_EQ(retirement.Poll(), GPUCompletionStatus::Pending);
        device.Fence(0)->Complete(point.value);
        EXPECT_EQ(retirement.Poll(), GPUCompletionStatus::Completed);
        EXPECT_TRUE(device.viewStates[0]->destroyed);
        EXPECT_EQ(device.viewStates[0]->destructionThread,
                  std::this_thread::get_id());
        cache.Shutdown();
        texture.Reset();
    }

    TEST(RenderLifetimeCutoverValidation, CompatibilityPoolReusePerformsOneWaitIdle)
    {
        FakeDevice device(RHIQueueCompletionMode::CompatibilityWaitIdle);
        RenderSubmissionTracker tracker;
        ASSERT_TRUE(tracker.Initialize(&device));
        RenderRetirementQueue retirement;
        ASSERT_TRUE(retirement.Initialize(&tracker));
        TransientResourcePool pool;
        pool.Initialize(&device, &tracker, &retirement);

        const RHITextureDesc desc = RHITextureDesc::Texture2D(
            8, 8, RHIFormat::RGBA8_UNORM,
            RHITextureUsage::ShaderResource);
        RHITexture* texture = pool.AcquireTexture(desc);
        ASSERT_NE(texture, nullptr);
        FakeCommandContext graphics(RHICommandQueueType::Graphics);
        GPUCompletionToken completion;
        ASSERT_TRUE(InsertGPUCompletionPoint(completion,
                                             tracker.Submit(&graphics)));
        pool.NotifySubmission(completion);
        pool.ReleaseTexture(texture);

        EXPECT_EQ(pool.AcquireTexture(desc), texture);
        EXPECT_EQ(device.waitIdleCount, 1u);
        pool.ReleaseTexture(texture);
        pool.Shutdown();
        EXPECT_EQ(retirement.Poll(),
                  GPUCompletionStatus::CompatibilityWaitIdle);
        EXPECT_EQ(device.waitIdleCount, 1u);
    }
} // namespace RVX::Tests
