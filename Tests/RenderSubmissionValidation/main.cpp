#include "Core/Log.h"
#include "Render/Context/FrameSynchronizer.h"
#include "Resources/RenderRetirementQueue.h"
#include "Resources/RenderSubmissionTracker.h"
#include "RHI/RHICapabilities.h"
#include "RHI/RHICommandContext.h"
#include "RHI/RHIDevice.h"
#include "RHI/RHIQueueTopology.h"

#include <gtest/gtest.h>

#include <array>
#include <limits>
#include <memory>
#include <span>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace RVX::Tests
{
    namespace
    {
        class ScopedCoreLogLevel final
        {
        public:
            explicit ScopedCoreLogLevel(spdlog::level::level_enum level)
                : m_previousLevel(Log::GetModuleLevel("CORE"))
            {
                Log::SetModuleLevel("CORE", level);
            }

            ~ScopedCoreLogLevel()
            {
                Log::SetModuleLevel("CORE", m_previousLevel);
            }

            ScopedCoreLogLevel(const ScopedCoreLogLevel&) = delete;
            ScopedCoreLogLevel& operator=(const ScopedCoreLogLevel&) = delete;

        private:
            spdlog::level::level_enum m_previousLevel = spdlog::level::info;
        };

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
            void Wait(uint64 value, uint64 = UINT64_MAX) override
            {
                ++waitCount;
                waitedValue = value;
                Complete(value);
            }

            uint64 AllocateValue() { return m_nextValue++; }
            void SetNextValue(uint64 value) { m_nextValue = value; }
            void SetCompletedValue(uint64 value) { m_completedValue = value; }
            uint64 GetNextValue() const { return m_nextValue; }
            void Complete(uint64 value)
            {
                m_completedValue = std::max(m_completedValue, value);
                m_nextValue = std::max(m_nextValue, value + 1);
            }

            uint32 waitCount = 0;
            uint64 waitedValue = 0;

        private:
            uint64 m_completedValue = 0;
            uint64 m_nextValue = 1;
        };

        RHICapabilities MakeCapabilities(RHIQueueCompletionMode mode,
                                         std::array<GPUQueueDomain, 3> mapping,
                                         uint8 activeDomainCount)
        {
            RHICapabilities capabilities;
            capabilities.backendType = RHIBackendType::DX12;
            capabilities.adapterName = "Fake";
            capabilities.driverVersion = "1";
            capabilities.supportsComputePipeline = true;
            capabilities.supportsDescriptorSets = true;
            capabilities.supportsDynamicDescriptorOffsets = true;
            capabilities.maxDescriptorSets = 4;
            capabilities.supportsExplicitResourceBarriers = true;
            capabilities.supportsDefaultQueueFenceSignal = mode == RHIQueueCompletionMode::NativeTimeline;
            capabilities.supportsExplicitQueueFenceSignal = mode == RHIQueueCompletionMode::NativeTimeline;
            capabilities.emulatesQueueFences = mode == RHIQueueCompletionMode::CompatibilityWaitIdle;
            capabilities.supportsAsyncCompute = mapping[1] != mapping[0];
            capabilities.dx12.resourceBindingTier = 2;
            capabilities.queueTopology.completionMode = mode;
            capabilities.queueTopology.logicalQueueDomains = mapping;
            capabilities.queueTopology.activeDomainCount = activeDomainCount;
            return capabilities;
        }

        class FakeDevice final : public IRHIDevice
        {
        public:
            explicit FakeDevice(RHICapabilities capabilities)
                : m_capabilities(std::move(capabilities))
                , m_backendType(m_capabilities.backendType)
            {
            }

            FakeDevice(RHICapabilities capabilities, RHIBackendType backendType)
                : m_capabilities(std::move(capabilities))
                , m_backendType(backendType)
            {
            }

            RHIBufferRef CreateBuffer(const RHIBufferDesc&) override { return {}; }
            RHITextureRef CreateTexture(const RHITextureDesc&) override { return {}; }
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
            RHICommandContextRef CreateCommandContext(RHICommandQueueType type) override
            {
                return RHICommandContextRef(new FakeCommandContext(type));
            }
            uint64 SubmitCommandContext(RHICommandContext* context, RHIFence* signalFence) override
            {
                ++submitCount;
                lastQueueType = static_cast<FakeCommandContext*>(context)->GetQueueType();
                lastSignalFence = signalFence;
                if (!signalFence)
                {
                    return 0;
                }
                auto* fence = static_cast<FakeFence*>(signalFence);
                if (failNextSubmit)
                {
                    failNextSubmit = false;
                    return 0;
                }
                return fence->AllocateValue();
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
            void WaitIdle() override
            {
                ++waitIdleCount;
            }
            void BeginFrame() override {}
            void EndFrame() override {}
            uint32 GetCurrentFrameIndex() const override { return 0; }
            RHIStagingBufferRef CreateStagingBuffer(const RHIStagingBufferDesc&) override { return {}; }
            RHIRingBufferRef CreateRingBuffer(const RHIRingBufferDesc&) override { return {}; }
            RHIMemoryStats GetMemoryStats() const override { return {}; }
            void BeginResourceGroup(const char*) override {}
            void EndResourceGroup() override {}
            const RHICapabilities& GetCapabilities() const override { return m_capabilities; }
            RHIBackendType GetBackendType() const override { return m_backendType; }

            FakeFence* GetFence(size_t index) const { return static_cast<FakeFence*>(fences[index].Get()); }

            RHICapabilities m_capabilities;
            std::vector<RHIFenceRef> fences;
            RHIFence* lastSignalFence = nullptr;
            RHICommandQueueType lastQueueType = RHICommandQueueType::Graphics;
            uint32 submitCount = 0;
            uint32 waitIdleCount = 0;
            bool failNextSubmit = false;

        private:
            RHIBackendType m_backendType = RHIBackendType::None;
        };

        struct RetirementProbeState
        {
            bool destroyed = false;
            std::thread::id destructionThread{};
        };

        class RetirementProbe final : public RefCounted
        {
        public:
            explicit RetirementProbe(std::shared_ptr<RetirementProbeState> state)
                : m_state(std::move(state))
            {
            }

            ~RetirementProbe() override
            {
                m_state->destroyed = true;
                m_state->destructionThread = std::this_thread::get_id();
            }

        private:
            std::shared_ptr<RetirementProbeState> m_state;
        };

        Ref<RefCounted> MakeRetirementProbe(
            const std::shared_ptr<RetirementProbeState>& state)
        {
            return Ref<RefCounted>(new RetirementProbe(state));
        }
    } // namespace

    TEST(RenderSubmissionValidation, PublicTopologyTypesHaveStableDefaultsAndLayout)
    {
        static_assert(std::is_same_v<std::underlying_type_t<GPUQueueDomain>, uint8>);
        static_assert(std::is_same_v<std::underlying_type_t<RHIQueueCompletionMode>, uint8>);
        static_assert(static_cast<uint8>(GPUQueueDomain::Graphics) == 0);
        static_assert(static_cast<uint8>(GPUQueueDomain::Compute) == 1);
        static_assert(static_cast<uint8>(GPUQueueDomain::Copy) == 2);
        static_assert(static_cast<uint8>(RHIQueueCompletionMode::None) == 0);
        static_assert(static_cast<uint8>(RHIQueueCompletionMode::NativeTimeline) == 1);
        static_assert(static_cast<uint8>(RHIQueueCompletionMode::CompatibilityWaitIdle) == 2);
        static_assert(static_cast<uint8>(GPUCompletionStatus::Completed) == 0);
        static_assert(static_cast<uint8>(GPUCompletionStatus::Pending) == 1);
        static_assert(static_cast<uint8>(GPUCompletionStatus::Lost) == 2);
        static_assert(static_cast<uint8>(GPUCompletionStatus::CompatibilityWaitIdle) == 3);
        static_assert(std::is_standard_layout_v<GPUCompletionPoint>);
        static_assert(std::is_trivially_copyable_v<GPUCompletionPoint>);
        static_assert(std::is_standard_layout_v<GPUCompletionToken>);
        static_assert(std::is_trivially_copyable_v<GPUCompletionToken>);
        static_assert(std::is_same_v<decltype(GPUCompletionToken{}.count), uint8>);

        const RHIQueueTopology topology;
        EXPECT_EQ(topology.completionMode, RHIQueueCompletionMode::None);
        EXPECT_EQ(topology.activeDomainCount, 0);
        const GPUCompletionPoint point;
        EXPECT_EQ(point.domain, GPUQueueDomain::Graphics);
        EXPECT_EQ(point.value, 0u);
        const GPUCompletionToken token;
        EXPECT_EQ(token.count, 0);
    }

    TEST(RenderSubmissionValidation, ValidatesDistinctCollapsedAndCompatibilityTopologies)
    {
        EXPECT_TRUE(ValidateRHICapabilities(MakeCapabilities(
            RHIQueueCompletionMode::NativeTimeline,
            {GPUQueueDomain::Graphics, GPUQueueDomain::Compute, GPUQueueDomain::Copy}, 3)));
        RHICapabilities collapsed = MakeCapabilities(
            RHIQueueCompletionMode::NativeTimeline,
            {GPUQueueDomain::Graphics, GPUQueueDomain::Graphics, GPUQueueDomain::Graphics}, 1);
        collapsed.backendType = RHIBackendType::Metal;
        EXPECT_TRUE(ValidateRHICapabilities(collapsed));

        RHICapabilities compatibility = MakeCapabilities(
            RHIQueueCompletionMode::CompatibilityWaitIdle,
            {GPUQueueDomain::Graphics, GPUQueueDomain::Graphics, GPUQueueDomain::Graphics}, 1);
        compatibility.backendType = RHIBackendType::DX11;
        compatibility.supportsComputePipeline = true;
        EXPECT_TRUE(ValidateRHICapabilities(compatibility));
    }

    TEST(RenderSubmissionValidation, RejectsFailClosedAndContradictoryTopologies)
    {
        RHICapabilities capabilities = MakeCapabilities(
            RHIQueueCompletionMode::NativeTimeline,
            {GPUQueueDomain::Graphics, GPUQueueDomain::Compute, GPUQueueDomain::Copy}, 3);

        capabilities.queueTopology.activeDomainCount = 2;
        EXPECT_FALSE(ValidateRHICapabilities(capabilities));
        capabilities.queueTopology.activeDomainCount = 3;
        capabilities.queueTopology.logicalQueueDomains[2] = static_cast<GPUQueueDomain>(99);
        EXPECT_FALSE(ValidateRHICapabilities(capabilities));
        capabilities.queueTopology.logicalQueueDomains[2] = GPUQueueDomain::Copy;
        capabilities.queueTopology.completionMode = static_cast<RHIQueueCompletionMode>(99);
        EXPECT_FALSE(ValidateRHICapabilities(capabilities));

        capabilities.queueTopology.completionMode = RHIQueueCompletionMode::NativeTimeline;
        capabilities.supportsAsyncCompute = false;
        EXPECT_FALSE(ValidateRHICapabilities(capabilities));

        capabilities = MakeCapabilities(
            RHIQueueCompletionMode::NativeTimeline,
            {GPUQueueDomain::Graphics, GPUQueueDomain::Copy, GPUQueueDomain::Copy}, 2);
        EXPECT_FALSE(ValidateRHICapabilities(capabilities));

        capabilities = MakeCapabilities(
            RHIQueueCompletionMode::NativeTimeline,
            {GPUQueueDomain::Graphics, GPUQueueDomain::Graphics, GPUQueueDomain::Compute}, 2);
        EXPECT_FALSE(ValidateRHICapabilities(capabilities));

        capabilities = RHICapabilities{};
        capabilities.backendType = RHIBackendType::DX12;
        EXPECT_FALSE(ValidateRHICapabilities(capabilities));
    }

    TEST(RenderSubmissionValidation, RejectsBackendTopologyIdentityContradictions)
    {
        RHICapabilities dx12 = MakeCapabilities(
            RHIQueueCompletionMode::NativeTimeline,
            {GPUQueueDomain::Graphics, GPUQueueDomain::Graphics, GPUQueueDomain::Graphics}, 1);
        EXPECT_FALSE(ValidateRHICapabilities(dx12));

        RHICapabilities dx11 = MakeCapabilities(
            RHIQueueCompletionMode::NativeTimeline,
            {GPUQueueDomain::Graphics, GPUQueueDomain::Graphics, GPUQueueDomain::Graphics}, 1);
        dx11.backendType = RHIBackendType::DX11;
        EXPECT_FALSE(ValidateRHICapabilities(dx11));

        RHICapabilities metal = MakeCapabilities(
            RHIQueueCompletionMode::NativeTimeline,
            {GPUQueueDomain::Graphics, GPUQueueDomain::Compute, GPUQueueDomain::Graphics}, 2);
        metal.backendType = RHIBackendType::Metal;
        EXPECT_FALSE(ValidateRHICapabilities(metal));

        RHICapabilities vulkan = MakeCapabilities(
            RHIQueueCompletionMode::CompatibilityWaitIdle,
            {GPUQueueDomain::Graphics, GPUQueueDomain::Graphics, GPUQueueDomain::Graphics}, 1);
        vulkan.backendType = RHIBackendType::Vulkan;
        vulkan.vulkan.apiVersion = 1;
        EXPECT_FALSE(ValidateRHICapabilities(vulkan));
    }

    TEST(RenderSubmissionValidation, TrackerRejectsMismatchedAndUndeclaredBackendIdentityBeforeAllocation)
    {
        FakeCommandContext graphics(RHICommandQueueType::Graphics);
        const auto expectRejectedBeforeAllocation = [&](RHICapabilities capabilities,
                                                        RHIBackendType deviceBackend)
        {
            FakeDevice device(std::move(capabilities), deviceBackend);
            RenderSubmissionTracker tracker;

            EXPECT_FALSE(tracker.Initialize(&device));
            EXPECT_TRUE(device.fences.empty());
            EXPECT_EQ(tracker.Submit(&graphics).value, 0u);
            EXPECT_EQ(device.submitCount, 0u);
        };

        RHICapabilities vulkan = MakeCapabilities(
            RHIQueueCompletionMode::NativeTimeline,
            {GPUQueueDomain::Graphics, GPUQueueDomain::Compute, GPUQueueDomain::Copy}, 3);
        vulkan.backendType = RHIBackendType::Vulkan;
        expectRejectedBeforeAllocation(vulkan, RHIBackendType::DX11);

        RHICapabilities metal = MakeCapabilities(
            RHIQueueCompletionMode::NativeTimeline,
            {GPUQueueDomain::Graphics, GPUQueueDomain::Graphics, GPUQueueDomain::Graphics}, 1);
        metal.backendType = RHIBackendType::Metal;
        expectRejectedBeforeAllocation(metal, RHIBackendType::DX11);

        const RHIBackendType unknownBackend = static_cast<RHIBackendType>(0xFF);
        RHICapabilities validDX12 = MakeCapabilities(
            RHIQueueCompletionMode::NativeTimeline,
            {GPUQueueDomain::Graphics, GPUQueueDomain::Compute, GPUQueueDomain::Copy}, 3);
        expectRejectedBeforeAllocation(validDX12, unknownBackend);
        expectRejectedBeforeAllocation(validDX12, RHIBackendType::None);
        expectRejectedBeforeAllocation(validDX12, RHIBackendType::Auto);

        RHICapabilities unknownCapabilities = validDX12;
        unknownCapabilities.backendType = unknownBackend;
        EXPECT_FALSE(ValidateRHICapabilities(unknownCapabilities));
        expectRejectedBeforeAllocation(unknownCapabilities, RHIBackendType::DX12);
    }

    TEST(RenderSubmissionValidation, TokenRejectsZeroSortsAndMergesOnlyWithinDomain)
    {
        GPUCompletionToken token;
        EXPECT_FALSE(InsertGPUCompletionPoint(token, {GPUQueueDomain::Graphics, 0}));
        EXPECT_TRUE(InsertGPUCompletionPoint(token, {GPUQueueDomain::Copy, 4}));
        EXPECT_TRUE(InsertGPUCompletionPoint(token, {GPUQueueDomain::Graphics, 9}));
        EXPECT_TRUE(InsertGPUCompletionPoint(token, {GPUQueueDomain::Compute, 3}));
        EXPECT_TRUE(InsertGPUCompletionPoint(token, {GPUQueueDomain::Copy, 7}));
        EXPECT_FALSE(InsertGPUCompletionPoint(token, {static_cast<GPUQueueDomain>(99), 1}));

        ASSERT_EQ(token.count, 3);
        EXPECT_EQ(token.points[0], (GPUCompletionPoint{GPUQueueDomain::Graphics, 9}));
        EXPECT_EQ(token.points[1], (GPUCompletionPoint{GPUQueueDomain::Compute, 3}));
        EXPECT_EQ(token.points[2], (GPUCompletionPoint{GPUQueueDomain::Copy, 7}));
    }

    TEST(RenderSubmissionValidation, TrackerOwnsOneFencePerDomainAndSelectsExactMappedFence)
    {
        FakeDevice device(MakeCapabilities(
            RHIQueueCompletionMode::NativeTimeline,
            {GPUQueueDomain::Graphics, GPUQueueDomain::Compute, GPUQueueDomain::Copy}, 3));
        RenderSubmissionTracker tracker;
        ASSERT_TRUE(tracker.Initialize(&device));
        ASSERT_EQ(device.fences.size(), 3u);

        FakeCommandContext graphics(RHICommandQueueType::Graphics);
        FakeCommandContext compute(RHICommandQueueType::Compute);
        FakeCommandContext copy(RHICommandQueueType::Copy);
        const GPUCompletionPoint graphics1 = tracker.Submit(&graphics);
        EXPECT_EQ(device.lastSignalFence, device.GetFence(0));
        const GPUCompletionPoint compute1 = tracker.Submit(&compute);
        EXPECT_EQ(device.lastSignalFence, device.GetFence(1));
        const GPUCompletionPoint copy1 = tracker.Submit(&copy);
        EXPECT_EQ(device.lastSignalFence, device.GetFence(2));
        const GPUCompletionPoint graphics2 = tracker.Submit(&graphics);

        EXPECT_EQ(graphics1, (GPUCompletionPoint{GPUQueueDomain::Graphics, 1}));
        EXPECT_EQ(compute1, (GPUCompletionPoint{GPUQueueDomain::Compute, 1}));
        EXPECT_EQ(copy1, (GPUCompletionPoint{GPUQueueDomain::Copy, 1}));
        EXPECT_EQ(graphics2, (GPUCompletionPoint{GPUQueueDomain::Graphics, 2}));
        EXPECT_EQ(tracker.GetLastSubmittedValue(GPUQueueDomain::Graphics), 2u);
        EXPECT_EQ(tracker.GetLastSubmittedValue(GPUQueueDomain::Compute), 1u);
        EXPECT_EQ(tracker.GetLastSubmittedValue(GPUQueueDomain::Copy), 1u);
    }

    TEST(RenderSubmissionValidation, TrackerDerivesFenceSelectionFromTheCommandContext)
    {
        FakeDevice device(MakeCapabilities(
            RHIQueueCompletionMode::NativeTimeline,
            {GPUQueueDomain::Graphics, GPUQueueDomain::Compute, GPUQueueDomain::Copy}, 3));
        RenderSubmissionTracker tracker;
        ASSERT_TRUE(tracker.Initialize(&device));

        FakeCommandContext compute(RHICommandQueueType::Compute);
        const GPUCompletionPoint point = tracker.Submit(&compute);

        EXPECT_EQ(point.domain, GPUQueueDomain::Compute);
        EXPECT_EQ(device.lastQueueType, RHICommandQueueType::Compute);
        EXPECT_EQ(device.lastSignalFence, device.GetFence(1));

        FakeCommandContext unknown(static_cast<RHICommandQueueType>(99));
        EXPECT_EQ(tracker.Submit(&unknown).value, 0u);
        EXPECT_EQ(device.submitCount, 1u);
    }

    TEST(RenderSubmissionValidation, CollapsedLogicalQueuesShareExactlyOnePhysicalTimeline)
    {
        RHICapabilities capabilities = MakeCapabilities(
            RHIQueueCompletionMode::NativeTimeline,
            {GPUQueueDomain::Graphics, GPUQueueDomain::Graphics, GPUQueueDomain::Graphics}, 1);
        capabilities.backendType = RHIBackendType::Metal;
        FakeDevice device(capabilities);
        RenderSubmissionTracker tracker;
        ASSERT_TRUE(tracker.Initialize(&device));
        ASSERT_EQ(device.fences.size(), 1u);

        FakeCommandContext graphics(RHICommandQueueType::Graphics);
        FakeCommandContext compute(RHICommandQueueType::Compute);
        FakeCommandContext copy(RHICommandQueueType::Copy);
        const GPUCompletionPoint graphicsPoint = tracker.Submit(&graphics);
        const GPUCompletionPoint computePoint = tracker.Submit(&compute);
        const GPUCompletionPoint copyPoint = tracker.Submit(&copy);

        EXPECT_EQ(graphicsPoint, (GPUCompletionPoint{GPUQueueDomain::Graphics, 1}));
        EXPECT_EQ(computePoint, (GPUCompletionPoint{GPUQueueDomain::Graphics, 2}));
        EXPECT_EQ(copyPoint, (GPUCompletionPoint{GPUQueueDomain::Graphics, 3}));
        EXPECT_EQ(device.lastSignalFence, device.GetFence(0));
    }

    TEST(RenderSubmissionValidation, MergeRejectsMalformedTokensWithoutMutatingDestination)
    {
        GPUCompletionToken destination;
        ASSERT_TRUE(InsertGPUCompletionPoint(
            destination, {GPUQueueDomain::Graphics, 7}));
        const GPUCompletionToken original = destination;

        GPUCompletionToken duplicateSource;
        duplicateSource.count = 2;
        duplicateSource.points[0] = {GPUQueueDomain::Copy, 2};
        duplicateSource.points[1] = {GPUQueueDomain::Copy, 3};
        EXPECT_FALSE(MergeGPUCompletionToken(destination, duplicateSource));
        EXPECT_EQ(destination.count, original.count);
        EXPECT_EQ(destination.points[0], original.points[0]);

        GPUCompletionToken malformedDestination = destination;
        malformedDestination.count = 4;
        const GPUCompletionToken malformedOriginal = malformedDestination;
        GPUCompletionToken validSource;
        ASSERT_TRUE(InsertGPUCompletionPoint(validSource, {GPUQueueDomain::Copy, 1}));
        EXPECT_FALSE(MergeGPUCompletionToken(malformedDestination, validSource));
        EXPECT_EQ(malformedDestination.count, malformedOriginal.count);
    }

    TEST(RenderSubmissionValidation, QueryRejectsMalformedTokensFailClosed)
    {
        FakeDevice device(MakeCapabilities(
            RHIQueueCompletionMode::NativeTimeline,
            {GPUQueueDomain::Graphics, GPUQueueDomain::Compute, GPUQueueDomain::Copy}, 3));
        RenderSubmissionTracker tracker;
        ASSERT_TRUE(tracker.Initialize(&device));

        GPUCompletionToken duplicateToken;
        duplicateToken.count = 2;
        duplicateToken.points[0] = {GPUQueueDomain::Graphics, 1};
        duplicateToken.points[1] = {GPUQueueDomain::Graphics, 2};
        EXPECT_EQ(tracker.Query(duplicateToken), GPUCompletionStatus::Lost);
        EXPECT_EQ(tracker.Query(GPUCompletionPoint{GPUQueueDomain::Graphics, 0}),
                  GPUCompletionStatus::Lost);
    }

    TEST(RenderSubmissionValidation, NativeTimelineOverflowIsReportedAsLost)
    {
        RHICapabilities capabilities = MakeCapabilities(
            RHIQueueCompletionMode::NativeTimeline,
            {GPUQueueDomain::Graphics, GPUQueueDomain::Graphics, GPUQueueDomain::Graphics}, 1);
        capabilities.backendType = RHIBackendType::Metal;
        FakeDevice device(capabilities);
        RenderSubmissionTracker tracker;
        ASSERT_TRUE(tracker.Initialize(&device));
        device.GetFence(0)->SetNextValue(std::numeric_limits<uint64>::max());

        FakeCommandContext graphics(RHICommandQueueType::Graphics);
        const GPUCompletionPoint last = tracker.Submit(&graphics);
        EXPECT_EQ(last.value, std::numeric_limits<uint64>::max());
        ASSERT_EQ(device.submitCount, 1u);
        ASSERT_EQ(tracker.GetLastSubmittedValue(GPUQueueDomain::Graphics), last.value);
        ASSERT_EQ(device.GetFence(0)->GetNextValue(), 0u);

        EXPECT_EQ(tracker.Submit(&graphics).value, 0u);
        EXPECT_EQ(device.submitCount, 1u)
            << "overflow must reject before backend work is submitted";
        EXPECT_EQ(tracker.GetLastSubmittedValue(GPUQueueDomain::Graphics), last.value);
        EXPECT_EQ(device.GetFence(0)->GetNextValue(), 0u);
        EXPECT_EQ(tracker.Query(last), GPUCompletionStatus::Lost);
        EXPECT_EQ(tracker.Wait(last), GPUCompletionStatus::Lost);
        EXPECT_EQ(device.GetFence(0)->waitCount, 0u);
    }

    TEST(RenderSubmissionValidation, TokenCompletionRequiresEveryDomainWithoutCrossDomainComparison)
    {
        FakeDevice device(MakeCapabilities(
            RHIQueueCompletionMode::NativeTimeline,
            {GPUQueueDomain::Graphics, GPUQueueDomain::Compute, GPUQueueDomain::Copy}, 3));
        RenderSubmissionTracker tracker;
        ASSERT_TRUE(tracker.Initialize(&device));

        FakeCommandContext graphics(RHICommandQueueType::Graphics);
        FakeCommandContext copy(RHICommandQueueType::Copy);
        GPUCompletionToken token;
        GPUCompletionPoint graphicsPoint{};
        for (uint32 i = 0; i < 5; ++i)
        {
            graphicsPoint = tracker.Submit(&graphics);
        }
        const GPUCompletionPoint copyPoint = tracker.Submit(&copy);
        ASSERT_TRUE(InsertGPUCompletionPoint(token, graphicsPoint));
        ASSERT_TRUE(InsertGPUCompletionPoint(token, copyPoint));

        device.GetFence(0)->Complete(graphicsPoint.value);
        EXPECT_EQ(tracker.Query(token), GPUCompletionStatus::Pending)
            << "Copy value 1 must remain pending even though Graphics reached 5";
        EXPECT_EQ(tracker.GetLastCompletedValue(GPUQueueDomain::Graphics), graphicsPoint.value);
        EXPECT_EQ(tracker.GetLastCompletedValue(GPUQueueDomain::Copy), 0u);
        device.GetFence(2)->Complete(copyPoint.value);
        EXPECT_EQ(tracker.Query(token), GPUCompletionStatus::Completed);
        EXPECT_EQ(tracker.GetLastCompletedValue(GPUQueueDomain::Copy), copyPoint.value);
    }

    TEST(RenderSubmissionValidation, TokenWaitRequiresEveryPhysicalDomain)
    {
        FakeDevice device(MakeCapabilities(
            RHIQueueCompletionMode::NativeTimeline,
            {GPUQueueDomain::Graphics, GPUQueueDomain::Compute, GPUQueueDomain::Copy}, 3));
        RenderSubmissionTracker tracker;
        ASSERT_TRUE(tracker.Initialize(&device));

        FakeCommandContext graphics(RHICommandQueueType::Graphics);
        FakeCommandContext copy(RHICommandQueueType::Copy);
        const GPUCompletionPoint graphicsPoint = tracker.Submit(&graphics);
        const GPUCompletionPoint copyPoint = tracker.Submit(&copy);
        GPUCompletionToken token;
        ASSERT_TRUE(InsertGPUCompletionPoint(token, graphicsPoint));
        ASSERT_TRUE(InsertGPUCompletionPoint(token, copyPoint));

        EXPECT_EQ(tracker.Wait(token), GPUCompletionStatus::Completed);
        EXPECT_EQ(device.GetFence(0)->waitCount, 1u);
        EXPECT_EQ(device.GetFence(0)->waitedValue, graphicsPoint.value);
        EXPECT_EQ(device.GetFence(2)->waitCount, 1u);
        EXPECT_EQ(device.GetFence(2)->waitedValue, copyPoint.value);
    }

    TEST(RenderSubmissionValidation, FailedNativeSubmissionMarksOnlyThatTimelineLost)
    {
        FakeDevice device(MakeCapabilities(
            RHIQueueCompletionMode::NativeTimeline,
            {GPUQueueDomain::Graphics, GPUQueueDomain::Compute, GPUQueueDomain::Copy}, 3));
        RenderSubmissionTracker tracker;
        ASSERT_TRUE(tracker.Initialize(&device));
        FakeCommandContext graphics(RHICommandQueueType::Graphics);

        device.failNextSubmit = true;
        const GPUCompletionPoint failed = tracker.Submit(&graphics);
        EXPECT_EQ(failed.value, 0u);
        EXPECT_EQ(tracker.Query(GPUCompletionPoint{GPUQueueDomain::Graphics, 1}),
                  GPUCompletionStatus::Lost);
        EXPECT_EQ(tracker.Query(GPUCompletionPoint{GPUQueueDomain::Copy, 1}),
                  GPUCompletionStatus::Pending);

        GPUCompletionToken token;
        token.count = 2;
        token.points[0] = {GPUQueueDomain::Graphics, 1};
        token.points[1] = {GPUQueueDomain::Copy, 1};
        EXPECT_EQ(tracker.Query(token), GPUCompletionStatus::Lost);
    }

    TEST(RenderSubmissionValidation, LostDomainPreemptsEarlierPendingDomain)
    {
        FakeDevice device(MakeCapabilities(
            RHIQueueCompletionMode::NativeTimeline,
            {GPUQueueDomain::Graphics, GPUQueueDomain::Compute, GPUQueueDomain::Copy}, 3));
        RenderSubmissionTracker tracker;
        ASSERT_TRUE(tracker.Initialize(&device));

        FakeCommandContext graphics(RHICommandQueueType::Graphics);
        FakeCommandContext copy(RHICommandQueueType::Copy);
        const GPUCompletionPoint graphicsPoint = tracker.Submit(&graphics);
        device.failNextSubmit = true;
        EXPECT_EQ(tracker.Submit(&copy).value, 0u);

        GPUCompletionToken token;
        token.count = 2;
        token.points[0] = graphicsPoint;
        token.points[1] = {GPUQueueDomain::Copy, 1};
        EXPECT_EQ(tracker.Query(token), GPUCompletionStatus::Lost);
        EXPECT_EQ(tracker.Wait(token), GPUCompletionStatus::Lost);
        EXPECT_EQ(device.GetFence(0)->waitCount, 0u);
    }

    TEST(RenderSubmissionValidation, CompatibilitySubmissionUsesExplicitWaitIdleWithoutFence)
    {
        RHICapabilities capabilities = MakeCapabilities(
            RHIQueueCompletionMode::CompatibilityWaitIdle,
            {GPUQueueDomain::Graphics, GPUQueueDomain::Graphics, GPUQueueDomain::Graphics}, 1);
        capabilities.backendType = RHIBackendType::DX11;
        FakeDevice device(capabilities);
        RenderSubmissionTracker tracker;
        ASSERT_TRUE(tracker.Initialize(&device));
        EXPECT_TRUE(device.fences.empty());

        FakeCommandContext graphics(RHICommandQueueType::Graphics);
        const GPUCompletionPoint point = tracker.Submit(&graphics);
        EXPECT_GT(point.value, 0u);
        EXPECT_EQ(device.lastSignalFence, nullptr);
        EXPECT_EQ(tracker.Query(point), GPUCompletionStatus::Pending);
        EXPECT_EQ(tracker.Wait(point), GPUCompletionStatus::CompatibilityWaitIdle);
        EXPECT_EQ(device.waitIdleCount, 1u);
        EXPECT_EQ(tracker.Query(point), GPUCompletionStatus::CompatibilityWaitIdle);
    }

    TEST(RenderSubmissionValidation, NativeFenceDeviceRemovalIsStickyLoss)
    {
        RHICapabilities capabilities = MakeCapabilities(
            RHIQueueCompletionMode::NativeTimeline,
            {GPUQueueDomain::Graphics, GPUQueueDomain::Graphics, GPUQueueDomain::Graphics}, 1);
        capabilities.backendType = RHIBackendType::Metal;
        FakeDevice device(capabilities);
        RenderSubmissionTracker tracker;
        ASSERT_TRUE(tracker.Initialize(&device));

        FakeCommandContext graphics(RHICommandQueueType::Graphics);
        const GPUCompletionPoint point = tracker.Submit(&graphics);
        ASSERT_GT(point.value, 0u);

        FakeFence* fence = device.GetFence(0);
        ASSERT_NE(fence, nullptr);
        fence->SetCompletedValue(UINT64_MAX);

        EXPECT_EQ(tracker.Query(point), GPUCompletionStatus::Lost);
        EXPECT_EQ(tracker.Wait(point), GPUCompletionStatus::Lost);
        EXPECT_EQ(fence->waitCount, 0u);
        EXPECT_EQ(tracker.GetLastCompletedValue(GPUQueueDomain::Graphics), 0u);

        fence->SetCompletedValue(point.value);
        EXPECT_EQ(tracker.GetLastCompletedValue(GPUQueueDomain::Graphics), 0u);
        EXPECT_EQ(tracker.Query(point), GPUCompletionStatus::Lost);
    }

    TEST(RenderSubmissionValidation, LostFrameSlotCannotBeReused)
    {
        const bool initializedLog = !Log::GetCoreLogger();
        if (initializedLog)
        {
            Log::Initialize();
        }

        RHICapabilities capabilities = MakeCapabilities(
            RHIQueueCompletionMode::NativeTimeline,
            {GPUQueueDomain::Graphics, GPUQueueDomain::Graphics, GPUQueueDomain::Graphics}, 1);
        capabilities.backendType = RHIBackendType::Metal;
        FakeDevice device(capabilities);
        FrameSynchronizer synchronizer;
        ASSERT_TRUE(synchronizer.Initialize(&device, 2));

        const GPUCompletionPoint point{GPUQueueDomain::Graphics, 7};
        synchronizer.SignalFrame(0, point);
        device.GetFence(0)->SetCompletedValue(UINT64_MAX);

        {
            ScopedCoreLogLevel suppressExpectedLost(spdlog::level::critical);
            EXPECT_FALSE(synchronizer.WaitForFrame(0));
            EXPECT_EQ(device.GetFence(0)->waitCount, 0u);
            EXPECT_EQ(synchronizer.GetFrameCompletionPoint(0), point);
            EXPECT_FALSE(synchronizer.IsFrameComplete(0));
            EXPECT_FALSE(synchronizer.WaitForAllFrames());
            synchronizer.Shutdown();
        }
        if (initializedLog)
        {
            Log::Shutdown();
        }
    }

    TEST(RenderSubmissionValidation, CompatibilityTokenPerformsOneExplicitWaitIdle)
    {
        RHICapabilities capabilities = MakeCapabilities(
            RHIQueueCompletionMode::CompatibilityWaitIdle,
            {GPUQueueDomain::Graphics, GPUQueueDomain::Graphics, GPUQueueDomain::Graphics}, 1);
        capabilities.backendType = RHIBackendType::DX11;
        FakeDevice device(capabilities);
        RenderSubmissionTracker tracker;
        ASSERT_TRUE(tracker.Initialize(&device));

        FakeCommandContext graphics(RHICommandQueueType::Graphics);
        FakeCommandContext copy(RHICommandQueueType::Copy);
        GPUCompletionToken token;
        ASSERT_TRUE(InsertGPUCompletionPoint(token, tracker.Submit(&graphics)));
        ASSERT_TRUE(InsertGPUCompletionPoint(token, tracker.Submit(&copy)));
        ASSERT_EQ(token.count, 1);

        EXPECT_EQ(tracker.Wait(token), GPUCompletionStatus::CompatibilityWaitIdle);
        EXPECT_EQ(device.waitIdleCount, 1u);
        EXPECT_EQ(tracker.Wait(token), GPUCompletionStatus::CompatibilityWaitIdle);
        EXPECT_EQ(device.waitIdleCount, 1u);
    }

    TEST(RenderSubmissionValidation, FrameSynchronizerStoresPointAndWaitsSharedGraphicsTimeline)
    {
        const bool initializedLog = !Log::GetCoreLogger();
        if (initializedLog)
        {
            Log::Initialize();
        }

        RHICapabilities capabilities = MakeCapabilities(
            RHIQueueCompletionMode::NativeTimeline,
            {GPUQueueDomain::Graphics, GPUQueueDomain::Graphics, GPUQueueDomain::Graphics}, 1);
        capabilities.backendType = RHIBackendType::Metal;
        FakeDevice device(capabilities);
        FrameSynchronizer synchronizer;
        ASSERT_TRUE(synchronizer.Initialize(&device, 2));
        ASSERT_EQ(device.fences.size(), 1u);

        const GPUCompletionPoint point{GPUQueueDomain::Graphics, 7};
        synchronizer.SignalFrame(1, point);
        EXPECT_EQ(synchronizer.GetFrameCompletionPoint(1), point);
        EXPECT_FALSE(synchronizer.IsFrameComplete(1));
        EXPECT_TRUE(synchronizer.WaitForFrame(1));
        EXPECT_EQ(device.GetFence(0)->waitCount, 1u);
        EXPECT_EQ(device.GetFence(0)->waitedValue, 7u);
        EXPECT_TRUE(synchronizer.IsFrameComplete(1));

        synchronizer.Shutdown();
        if (initializedLog)
        {
            Log::Shutdown();
        }
    }

    TEST(RenderSubmissionValidation, RetirementWaitsForEveryDomainAndPublishesDiagnostics)
    {
        FakeDevice device(MakeCapabilities(
            RHIQueueCompletionMode::NativeTimeline,
            {GPUQueueDomain::Graphics, GPUQueueDomain::Compute, GPUQueueDomain::Copy}, 3));
        RenderSubmissionTracker tracker;
        ASSERT_TRUE(tracker.Initialize(&device));
        RenderRetirementQueue queue;
        ASSERT_TRUE(queue.Initialize(&tracker));

        FakeCommandContext graphics(RHICommandQueueType::Graphics);
        FakeCommandContext copy(RHICommandQueueType::Copy);
        const GPUCompletionPoint graphicsPoint = tracker.Submit(&graphics);
        const GPUCompletionPoint copyPoint = tracker.Submit(&copy);
        GPUCompletionToken token;
        ASSERT_TRUE(InsertGPUCompletionPoint(token, graphicsPoint));
        ASSERT_TRUE(InsertGPUCompletionPoint(token, copyPoint));

        const auto state = std::make_shared<RetirementProbeState>();
        RenderRetirementEntry entry{token, MakeRetirementProbe(state), 4096};
        ASSERT_TRUE(queue.Enqueue(std::move(entry)));
        EXPECT_FALSE(entry.object);

        const RenderRetirementDiagnostics pending = queue.GetDiagnostics();
        EXPECT_EQ(pending.entryCount, 1u);
        EXPECT_EQ(pending.estimatedBytes, 4096u);
        EXPECT_EQ(pending.oldestPendingCompletionValues[0], graphicsPoint.value);
        EXPECT_EQ(pending.oldestPendingCompletionValues[1], 0u);
        EXPECT_EQ(pending.oldestPendingCompletionValues[2], copyPoint.value);

        device.GetFence(0)->Complete(graphicsPoint.value);
        EXPECT_EQ(queue.Poll(), GPUCompletionStatus::Pending);
        EXPECT_FALSE(state->destroyed);
        const RenderRetirementDiagnostics partiallyComplete = queue.GetDiagnostics();
        EXPECT_EQ(partiallyComplete.oldestPendingCompletionValues[0], 0u);
        EXPECT_EQ(partiallyComplete.oldestPendingCompletionValues[2], copyPoint.value);

        device.GetFence(2)->Complete(copyPoint.value);
        EXPECT_EQ(queue.Poll(), GPUCompletionStatus::Completed);
        EXPECT_TRUE(state->destroyed);
        EXPECT_EQ(state->destructionThread, std::this_thread::get_id());
        EXPECT_EQ(queue.GetDiagnostics().entryCount, 0u);
    }

    TEST(RenderSubmissionValidation, RetirementHonorsSameDomainMaximum)
    {
        RHICapabilities capabilities = MakeCapabilities(
            RHIQueueCompletionMode::NativeTimeline,
            {GPUQueueDomain::Graphics, GPUQueueDomain::Graphics, GPUQueueDomain::Graphics}, 1);
        capabilities.backendType = RHIBackendType::Metal;
        FakeDevice device(capabilities);
        RenderSubmissionTracker tracker;
        ASSERT_TRUE(tracker.Initialize(&device));
        RenderRetirementQueue queue;
        ASSERT_TRUE(queue.Initialize(&tracker));

        FakeCommandContext graphics(RHICommandQueueType::Graphics);
        GPUCompletionToken earlier;
        GPUCompletionToken later;
        const GPUCompletionPoint first = tracker.Submit(&graphics);
        const GPUCompletionPoint second = tracker.Submit(&graphics);
        ASSERT_TRUE(InsertGPUCompletionPoint(earlier, first));
        ASSERT_TRUE(InsertGPUCompletionPoint(later, second));
        ASSERT_TRUE(MergeGPUCompletionToken(earlier, later));
        ASSERT_EQ(earlier.count, 1u);
        ASSERT_EQ(earlier.points[0], second);

        const auto state = std::make_shared<RetirementProbeState>();
        ASSERT_TRUE(queue.Enqueue({earlier, MakeRetirementProbe(state), 64}));
        device.GetFence(0)->Complete(first.value);
        EXPECT_EQ(queue.Poll(), GPUCompletionStatus::Pending);
        EXPECT_FALSE(state->destroyed);
        device.GetFence(0)->Complete(second.value);
        EXPECT_EQ(queue.Poll(), GPUCompletionStatus::Completed);
        EXPECT_TRUE(state->destroyed);
    }

    TEST(RenderSubmissionValidation, ZeroPointRetirementReleasesOnRenderThread)
    {
        RenderSubmissionTracker uninitializedTracker;
        RenderRetirementQueue uninitializedQueue;
        EXPECT_FALSE(uninitializedQueue.Initialize(&uninitializedTracker));

        FakeDevice device(MakeCapabilities(
            RHIQueueCompletionMode::NativeTimeline,
            {GPUQueueDomain::Graphics, GPUQueueDomain::Compute, GPUQueueDomain::Copy}, 3));
        RenderSubmissionTracker tracker;
        ASSERT_TRUE(tracker.Initialize(&device));
        RenderRetirementQueue queue;
        ASSERT_TRUE(queue.Initialize(&tracker));

        const auto state = std::make_shared<RetirementProbeState>();
        ASSERT_TRUE(queue.Enqueue({{}, MakeRetirementProbe(state), 16}));
        EXPECT_TRUE(state->destroyed);
        EXPECT_EQ(state->destructionThread, std::this_thread::get_id());
        EXPECT_EQ(queue.GetDiagnostics().entryCount, 0u);
    }

    TEST(RenderSubmissionValidation, CompatibilityRetirementPerformsOneBoundedWaitIdle)
    {
        RHICapabilities capabilities = MakeCapabilities(
            RHIQueueCompletionMode::CompatibilityWaitIdle,
            {GPUQueueDomain::Graphics, GPUQueueDomain::Graphics, GPUQueueDomain::Graphics}, 1);
        capabilities.backendType = RHIBackendType::DX11;
        FakeDevice device(capabilities);
        RenderSubmissionTracker tracker;
        ASSERT_TRUE(tracker.Initialize(&device));
        RenderRetirementQueue queue;
        ASSERT_TRUE(queue.Initialize(&tracker));

        FakeCommandContext graphics(RHICommandQueueType::Graphics);
        FakeCommandContext copy(RHICommandQueueType::Copy);
        GPUCompletionToken token;
        ASSERT_TRUE(InsertGPUCompletionPoint(token, tracker.Submit(&graphics)));
        ASSERT_TRUE(InsertGPUCompletionPoint(token, tracker.Submit(&copy)));
        const auto state = std::make_shared<RetirementProbeState>();
        ASSERT_TRUE(queue.Enqueue({token, MakeRetirementProbe(state), 128}));

        EXPECT_EQ(queue.Poll(), GPUCompletionStatus::CompatibilityWaitIdle);
        EXPECT_EQ(device.waitIdleCount, 1u);
        EXPECT_TRUE(state->destroyed);
        EXPECT_EQ(queue.Poll(), GPUCompletionStatus::Completed);
        EXPECT_EQ(device.waitIdleCount, 1u);
    }

    TEST(RenderSubmissionValidation, DeviceLostRetirementRequiresExplicitLostTeardown)
    {
        RHICapabilities capabilities = MakeCapabilities(
            RHIQueueCompletionMode::NativeTimeline,
            {GPUQueueDomain::Graphics, GPUQueueDomain::Graphics, GPUQueueDomain::Graphics}, 1);
        capabilities.backendType = RHIBackendType::Metal;
        FakeDevice device(capabilities);
        RenderSubmissionTracker tracker;
        ASSERT_TRUE(tracker.Initialize(&device));
        RenderRetirementQueue queue;
        ASSERT_TRUE(queue.Initialize(&tracker));

        FakeCommandContext graphics(RHICommandQueueType::Graphics);
        GPUCompletionToken token;
        ASSERT_TRUE(InsertGPUCompletionPoint(token, tracker.Submit(&graphics)));
        const auto state = std::make_shared<RetirementProbeState>();
        ASSERT_TRUE(queue.Enqueue({token, MakeRetirementProbe(state), 512}));
        device.GetFence(0)->SetCompletedValue(UINT64_MAX);

        EXPECT_EQ(queue.Poll(), GPUCompletionStatus::Lost);
        EXPECT_FALSE(state->destroyed);
        EXPECT_EQ(queue.GetDiagnostics().entryCount, 1u);
        EXPECT_EQ(queue.ForceDeviceLostTeardown(), GPUCompletionStatus::Lost);
        EXPECT_TRUE(state->destroyed);
        EXPECT_EQ(state->destructionThread, std::this_thread::get_id());
        EXPECT_EQ(queue.GetDiagnostics().entryCount, 0u);
    }

    TEST(RenderSubmissionValidation, RetirementMutationIsRejectedOffRenderThread)
    {
        RHICapabilities capabilities = MakeCapabilities(
            RHIQueueCompletionMode::NativeTimeline,
            {GPUQueueDomain::Graphics, GPUQueueDomain::Graphics, GPUQueueDomain::Graphics}, 1);
        capabilities.backendType = RHIBackendType::Metal;
        FakeDevice device(capabilities);
        RenderSubmissionTracker tracker;
        ASSERT_TRUE(tracker.Initialize(&device));
        RenderRetirementQueue queue;
        ASSERT_TRUE(queue.Initialize(&tracker));

        FakeCommandContext graphics(RHICommandQueueType::Graphics);
        GPUCompletionToken token;
        ASSERT_TRUE(InsertGPUCompletionPoint(token, tracker.Submit(&graphics)));
        const auto retainedState = std::make_shared<RetirementProbeState>();
        ASSERT_TRUE(queue.Enqueue({token, MakeRetirementProbe(retainedState), 256}));

        const auto rejectedState = std::make_shared<RetirementProbeState>();
        RenderRetirementEntry rejected{token, MakeRetirementProbe(rejectedState), 128};
        bool enqueueResult = true;
        GPUCompletionStatus pollResult = GPUCompletionStatus::Completed;
        GPUCompletionStatus teardownResult = GPUCompletionStatus::Completed;
        std::thread worker([&]
        {
            enqueueResult = queue.Enqueue(std::move(rejected));
            pollResult = queue.Poll();
            teardownResult = queue.ForceDeviceLostTeardown();
        });
        worker.join();

        EXPECT_FALSE(enqueueResult);
        EXPECT_EQ(pollResult, GPUCompletionStatus::Lost);
        EXPECT_EQ(teardownResult, GPUCompletionStatus::Lost);
        EXPECT_TRUE(rejected.object);
        EXPECT_FALSE(rejectedState->destroyed);
        EXPECT_FALSE(retainedState->destroyed);
        EXPECT_EQ(queue.GetDiagnostics().entryCount, 1u);

        rejected.object.Reset();
        EXPECT_EQ(rejectedState->destructionThread, std::this_thread::get_id());
        EXPECT_EQ(queue.ForceDeviceLostTeardown(), GPUCompletionStatus::Lost);
        EXPECT_TRUE(retainedState->destroyed);
        EXPECT_EQ(retainedState->destructionThread, std::this_thread::get_id());
    }
} // namespace RVX::Tests
