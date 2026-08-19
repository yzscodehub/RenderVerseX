#include "RHI/RHICapabilities.h"
#include "RHI/RHIBuffer.h"
#include "RHI/RHICommandContext.h"
#include "RHI/RHIDevice.h"
#include "RHI/RHIDeviceStatus.h"
#include "RHI/RHINativeSurface.h"
#include "RHI/RHITexture.h"
#include "RHI/RHIUpload.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace RVX::Tests
{
    static_assert(static_cast<uint8>(RHIDeviceRuntimeStatus::Ready) == 0);
    static_assert(static_cast<uint8>(RHIDeviceRuntimeStatus::DeviceLost) == 1);
    static_assert(static_cast<uint8>(RHIDeviceRuntimeStatus::FatalError) == 2);
    static_assert(static_cast<uint8>(RHIDeviceFaultOperation::None) == 0);
    static_assert(static_cast<uint8>(RHIDeviceFaultOperation::Shutdown) == 10);
    static_assert(static_cast<uint8>(RHICapabilityFeature::ComputePipeline) == 0);
    static_assert(static_cast<uint8>(RHICapabilityFeature::DescriptorSets) == 1);
    static_assert(static_cast<uint8>(RHICapabilityFeature::ExplicitResourceBarriers) == 2);
    static_assert(static_cast<uint8>(RHICapabilityFeature::QueueSynchronization) == 3);
    static_assert(static_cast<uint8>(RHICapabilityFeature::AsyncCompute) == 4);
    static_assert(static_cast<uint8>(RHICapabilityFeature::IndirectDrawCount) == 5);
    static_assert(static_cast<uint8>(RHICapabilityFeature::RayTracing) == 6);
    static_assert(static_cast<uint8>(RHICapabilityFeature::BindlessResources) == 7);
    static_assert(static_cast<uint8>(RHICapabilityFeature::QuerySupport) == 8);
    static_assert(static_cast<uint8>(RHICapabilityFeature::MemoryBudget) == 9);
    static_assert(static_cast<uint8>(RHICapabilityFeature::ExplicitHeapManagement) == 10);
    static_assert(static_cast<uint8>(RHICapabilityFeature::IndexedIndirectExecution) == 11);

    namespace
    {
        class FakeRHIDevice final : public IRHIDevice
        {
        public:
            explicit FakeRHIDevice(RHICapabilities capabilities)
                : m_capabilities(std::move(capabilities))
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
            RHICommandContextRef CreateCommandContext(RHICommandQueueType) override { return {}; }
            uint64 SubmitCommandContext(RHICommandContext*, RHIFence*) override { return 0; }
            uint64 SubmitCommandContexts(std::span<RHICommandContext* const>, RHIFence*) override { return 0; }
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
            const RHICapabilities& GetCapabilities() const override { return m_capabilities; }
            RHIBackendType GetBackendType() const override { return m_capabilities.backendType; }

        private:
            RHICapabilities m_capabilities;
        };

        class QueueContractContext final : public RHICommandContext
        {
        public:
            explicit QueueContractContext(RHICommandQueueType queueType)
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
            void SetVertexBuffers(uint32,
                                  std::span<RHIBuffer* const>,
                                  std::span<const uint64> = {}) override {}
            void SetIndexBuffer(RHIBuffer*, RHIFormat, uint64 = 0) override {}
            void SetDescriptorSet(uint32,
                                  RHIDescriptorSet*,
                                  std::span<const uint32> = {}) override {}
            void SetPushConstants(const void*, uint32, uint32 = 0) override {}
            void SetViewport(const RHIViewport&) override {}
            void SetViewports(std::span<const RHIViewport>) override {}
            void SetScissor(const RHIRect&) override {}
            void SetScissors(std::span<const RHIRect>) override {}
            void Draw(uint32, uint32 = 1, uint32 = 0, uint32 = 0) override {}
            void DrawIndexed(uint32,
                             uint32 = 1,
                             uint32 = 0,
                             int32 = 0,
                             uint32 = 0) override {}
            void DrawIndirect(RHIBuffer*, uint64, uint32, uint32) override {}
            void DrawIndexedIndirect(RHIBuffer*, uint64, uint32, uint32) override {}
            void Dispatch(uint32, uint32, uint32) override {}
            void DispatchIndirect(RHIBuffer*, uint64) override {}
            void CopyBuffer(RHIBuffer*, RHIBuffer*, uint64, uint64, uint64) override {}
            void CopyTexture(RHITexture*,
                             RHITexture*,
                             const RHITextureCopyDesc& = {}) override {}
            void CopyBufferToTexture(
                RHIBuffer*,
                RHITexture*,
                const RHIBufferTextureCopyDesc&) override {}
            void CopyTextureToBuffer(
                RHITexture*,
                RHIBuffer*,
                const RHIBufferTextureCopyDesc&) override {}
            void BeginQuery(RHIQueryPool*, uint32) override {}
            void EndQuery(RHIQueryPool*, uint32) override {}
            void WriteTimestamp(RHIQueryPool*, uint32) override {}
            void ResolveQueries(RHIQueryPool*,
                                uint32,
                                uint32,
                                RHIBuffer*,
                                uint64) override {}
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

        class DescriptorContractBuffer final : public RHIBuffer
        {
        public:
            uint64 GetSize() const override { return 256; }
            RHIBufferUsage GetUsage() const override { return RHIBufferUsage::Constant; }
            RHIMemoryType GetMemoryType() const override { return RHIMemoryType::Upload; }
            uint32 GetStride() const override { return 0; }
            void* Map() override { return nullptr; }
            void Unmap() override {}
        };

        class QueryContractPool final : public RHIQueryPool
        {
        public:
            QueryContractPool(
                RHIQueryType type,
                RHICommandQueueType queueType,
                uint32 count,
                uint64 timestampFrequency,
                uint8 timestampValidBits)
                : RHIQueryPool(queueType, timestampValidBits)
                , m_type(type)
                , m_count(count)
                , m_timestampFrequency(timestampFrequency)
            {
            }

            RHIQueryType GetType() const override { return m_type; }
            uint32 GetCount() const override { return m_count; }
            uint64 GetTimestampFrequency() const override
            {
                return m_timestampFrequency;
            }

        private:
            RHIQueryType m_type = RHIQueryType::Timestamp;
            uint32 m_count = 0;
            uint64 m_timestampFrequency = 0;
        };

        class QueryResolveContractBuffer final : public RHIBuffer
        {
        public:
            QueryResolveContractBuffer(uint64 size, RHIBufferUsage usage)
                : m_size(size)
                , m_usage(usage)
            {
            }

            uint64 GetSize() const override { return m_size; }
            RHIBufferUsage GetUsage() const override { return m_usage; }
            RHIMemoryType GetMemoryType() const override
            {
                return RHIMemoryType::Readback;
            }
            uint32 GetStride() const override { return 0; }
            void* Map() override { return nullptr; }
            void Unmap() override {}

        private:
            uint64 m_size = 0;
            RHIBufferUsage m_usage = RHIBufferUsage::None;
        };

        class IndirectExecutionContractBuffer final : public RHIBuffer
        {
        public:
            IndirectExecutionContractBuffer(uint64 size, RHIBufferUsage usage)
                : m_size(size), m_usage(usage)
            {
            }

            uint64 GetSize() const override { return m_size; }
            RHIBufferUsage GetUsage() const override { return m_usage; }
            RHIMemoryType GetMemoryType() const override { return RHIMemoryType::Default; }
            uint32 GetStride() const override { return 0; }
            void* Map() override { return nullptr; }
            void Unmap() override {}

        private:
            uint64 m_size = 0;
            RHIBufferUsage m_usage = RHIBufferUsage::None;
        };

        class LegacyStagingBuffer final : public RHIStagingBuffer
        {
        public:
            void* Map(uint64 = 0, uint64 = RVX_WHOLE_SIZE) override
            {
                ++m_mapCount;
                return m_storage.data();
            }

            void Unmap() override
            {
                ++m_unmapCount;
            }

            uint64 GetSize() const override { return m_storage.size(); }
            RHIBuffer* GetBuffer() const override { return nullptr; }

            uint32 m_mapCount = 0;
            uint32 m_unmapCount = 0;

        private:
            std::array<uint8, 16> m_storage{};
        };

        class LegacyMappedWriteBuffer : public RHIBuffer
        {
        public:
            explicit LegacyMappedWriteBuffer(
                RHIMemoryType memoryType = RHIMemoryType::Upload)
                : m_memoryType(memoryType)
            {
            }

            uint64 GetSize() const override { return m_storage.size(); }
            RHIBufferUsage GetUsage() const override { return RHIBufferUsage::CopyDst; }
            RHIMemoryType GetMemoryType() const override { return m_memoryType; }
            uint32 GetStride() const override { return 1; }

            void* Map() override
            {
                ++m_mapCount;
                return m_storage.data();
            }

            void Unmap() override
            {
                ++m_unmapCount;
            }

            std::array<uint8, 16> m_storage{};
            uint32 m_mapCount = 0;
            uint32 m_unmapCount = 0;

        private:
            RHIMemoryType m_memoryType = RHIMemoryType::Upload;
        };

        class FailingLegacyMappedWriteBuffer final
            : public LegacyMappedWriteBuffer
        {
        public:
            bool CommitMappedWrite() override
            {
                ++commitCount;
                if (failCommit)
                    return false;
                return RHIBuffer::CommitMappedWrite();
            }

            bool failCommit = true;
            uint32 commitCount = 0;
        };

        class PoisonedCancelMappedWriteBuffer final
            : public LegacyMappedWriteBuffer
        {
        protected:
            bool CancelMappedWriteRangeImpl(uint64, uint64) override
            {
                ++cancelCount;
                return false;
            }

        public:
            uint32 cancelCount = 0;
        };

        class DescriptorContractLayout final : public RHIDescriptorSetLayout
        {
        public:
            explicit DescriptorContractLayout(RHIDescriptorSetLayoutDesc desc)
                : m_entries(std::move(desc.entries))
            {
            }

            const std::vector<RHIBindingLayoutEntry>& GetEntries() const override
            {
                return m_entries;
            }

        private:
            std::vector<RHIBindingLayoutEntry> m_entries;
        };

        class DescriptorContractSet final : public RHIDescriptorSet
        {
        public:
            explicit DescriptorContractSet(const RHIDescriptorSetDesc& desc)
                : RHIDescriptorSet(desc)
            {
            }
        };

        RHICapabilities MakeValidCapabilities(RHIBackendType backend)
        {
            RHICapabilities capabilities;
            capabilities.backendType = backend;
            capabilities.adapterName = std::string(ToString(backend)) + " Test Adapter";
            capabilities.driverVersion = "TestDriver.1";
            capabilities.supportsComputePipeline = true;
            capabilities.supportsDescriptorSets = true;
            capabilities.supportsDynamicDescriptorOffsets = true;
            capabilities.maxDescriptorSets = 4;
            capabilities.supportsExplicitResourceBarriers = true;
            capabilities.supportsDefaultQueueFenceSignal = true;
            capabilities.queueTopology.logicalQueueDomains = {
                GPUQueueDomain::Graphics,
                GPUQueueDomain::Graphics,
                GPUQueueDomain::Graphics,
            };
            capabilities.queueTopology.activeDomainCount = 1;

            if (backend == RHIBackendType::DX11 || backend == RHIBackendType::OpenGL)
            {
                capabilities.queueTopology.completionMode =
                    RHIQueueCompletionMode::CompatibilityWaitIdle;
                capabilities.emulatesQueueFences = true;
            }
            else
            {
                capabilities.queueTopology.completionMode = RHIQueueCompletionMode::NativeTimeline;
            }

            if (backend == RHIBackendType::DX12)
            {
                capabilities.dx12.resourceBindingTier = 2;
                capabilities.supportsAsyncCompute = true;
                capabilities.queueTopology.logicalQueueDomains = {
                    GPUQueueDomain::Graphics,
                    GPUQueueDomain::Compute,
                    GPUQueueDomain::Copy,
                };
                capabilities.queueTopology.activeDomainCount = 3;
            }
            else if (backend == RHIBackendType::Vulkan)
            {
                capabilities.vulkan.apiVersion = 1;
            }
            else if (backend == RHIBackendType::OpenGL)
            {
                capabilities.opengl.majorVersion = 4;
                capabilities.opengl.minorVersion = 5;
            }

            return capabilities;
        }

        void EnableIndexedIndirectExecution(
            RHICapabilities& capabilities,
            bool countBuffer = true)
        {
            RHIIndexedIndirectExecutionCapabilities& execution =
                capabilities.indexedIndirectExecution;
            execution.supportsFixedCount = true;
            execution.supportsCountBuffer = countBuffer;
            execution.supportsFirstInstance = true;
            execution.requiresExactCommandStride = false;
            execution.indexedCommandSize = sizeof(IndirectDrawIndexedCommand);
            execution.minCommandStride = sizeof(IndirectDrawIndexedCommand);
            execution.commandStrideAlignment = 4;
            execution.argumentOffsetAlignment = 4;
            execution.countOffsetAlignment = 4;
            execution.maxDrawCount = 4;
            execution.countValueSize = sizeof(uint32);
            execution.requiredArgumentState = RHIResourceState::IndirectArgument;
            execution.requiredCountState = RHIResourceState::IndirectArgument;
            capabilities.supportsIndirectDrawCount = countBuffer;
        }

        const RHICapabilityReportEntry* FindReportEntry(const RHICapabilityReport& report,
                                                        RHICapabilityFeature feature)
        {
            const auto it = std::find_if(report.entries.begin(),
                                         report.entries.end(),
                                         [feature](const RHICapabilityReportEntry& entry)
                                         {
                                             return entry.feature == feature;
                                         });
            return it != report.entries.end() ? &(*it) : nullptr;
        }

        bool HasMissingRequirement(const RHICapabilityReport& report, const std::string& requirement)
        {
            return std::find(report.renderGraphBaselineMissingRequirements.begin(),
                             report.renderGraphBaselineMissingRequirements.end(),
                             requirement) != report.renderGraphBaselineMissingRequirements.end();
        }

        template <typename T>
        concept HasWindowHandleField = requires(T value) { value.windowHandle; };

        template <typename T>
        concept HasWidthField = requires(T value) { value.width; };

        template <typename T>
        concept HasHeightField = requires(T value) { value.height; };

        template <typename T>
        concept HasFormatField = requires(T value) { value.format; };

        template <typename T>
        concept HasVsyncField = requires(T value) { value.vsync; };

        NativeSurfaceDesc MakeSurface(NativeSurfacePlatform platform)
        {
            NativeSurfaceDesc surface;
            surface.platform = platform;
            surface.nativeWindow = 0x1000;
            surface.nativeDisplay = 0x2000;
            surface.nativeLayer = 0x3000;
            surface.backendWindow = 0x4000;
            surface.width = 1280;
            surface.height = 720;
            surface.contentScale = 1.0f;
            surface.generation = 1;
            return surface;
        }
    } // namespace

    TEST(RHIContractValidation,
         TimestampQueryContractValidatesPoolQueueRangeAndResolveDestination)
    {
        RHIQueryPoolDesc desc;
        desc.type = RHIQueryType::Timestamp;
        desc.queueType = RHICommandQueueType::Graphics;
        desc.count = 2;
        EXPECT_TRUE(ValidateRHIQueryPoolDesc(desc));

        desc.queueType = RHICommandQueueType::Compute;
        EXPECT_FALSE(ValidateRHIQueryPoolDesc(desc));
        EXPECT_NE(std::string(ValidateRHIQueryPoolDesc(desc).message).find("Graphics"),
                  std::string::npos);

        desc.queueType = static_cast<RHICommandQueueType>(0xFF);
        EXPECT_FALSE(ValidateRHIQueryPoolDesc(desc));

        QueryContractPool pool(
            RHIQueryType::Timestamp,
            RHICommandQueueType::Graphics,
            2,
            1000000000ull,
            64);
        EXPECT_EQ(pool.GetQueueType(), RHICommandQueueType::Graphics);
        EXPECT_EQ(pool.GetTimestampValidBits(), 64);
        EXPECT_TRUE(ValidateRHIQueryPoolMetadata(
            pool,
            RHIQueryType::Timestamp,
            RHICommandQueueType::Graphics));
        EXPECT_FALSE(ValidateRHIQueryPoolMetadata(
            pool,
            RHIQueryType::Timestamp,
            RHICommandQueueType::Compute));
        EXPECT_FALSE(ValidateRHIQueryPoolMetadata(
            pool,
            RHIQueryType::Occlusion,
            RHICommandQueueType::Graphics));

        EXPECT_TRUE(ValidateRHIQueryRange(pool, 0, 2));
        EXPECT_FALSE(ValidateRHIQueryRange(pool, 2, 1));
        EXPECT_FALSE(ValidateRHIQueryRange(pool, 1, 2));
        EXPECT_FALSE(ValidateRHIQueryRange(pool, 0, 0));

        QueryResolveContractBuffer resolveBuffer(16, RHIBufferUsage::CopyDst);
        EXPECT_TRUE(ValidateRHIQueryResolveDestination(
            pool,
            0,
            2,
            resolveBuffer,
            0));
        EXPECT_FALSE(ValidateRHIQueryResolveDestination(
            pool,
            0,
            2,
            resolveBuffer,
            8));
        EXPECT_FALSE(ValidateRHIQueryResolveDestination(
            pool,
            0,
            1,
            resolveBuffer,
            4));

        QueryResolveContractBuffer wrongUsageBuffer(16, RHIBufferUsage::CopySrc);
        EXPECT_FALSE(ValidateRHIQueryResolveDestination(
            pool,
            0,
            1,
            wrongUsageBuffer,
            0));
    }

    TEST(RHIContractValidation, TimestampElapsedDeltaUsesDeclaredModuloWidth)
    {
        EXPECT_EQ(CalculateRHITimestampElapsedDelta(0xFEu, 0x02u, 8), 4u);
        EXPECT_EQ(CalculateRHITimestampElapsedDelta(
                      std::numeric_limits<uint64>::max() - 1,
                      1,
                      64),
                  3u);
        EXPECT_EQ(CalculateRHITimestampElapsedDelta(1, 2, 0), 0u);
        EXPECT_EQ(CalculateRHITimestampElapsedDelta(1, 2, 65), 0u);
    }

    TEST(RHIContractValidation,
         TimestampCapabilityRequiresCoherentGraphicsFrequency)
    {
        RHICapabilities capabilities = MakeValidCapabilities(RHIBackendType::DX12);
        EXPECT_TRUE(ValidateRHICapabilities(capabilities));

        capabilities.supportsTimestampQueries = true;
        EXPECT_FALSE(ValidateRHICapabilities(capabilities));
        EXPECT_NE(ValidateRHICapabilities(capabilities).message.find("timestamp query support"),
                  std::string::npos);

        capabilities.timestampFrequency = 1000000000ull;
        EXPECT_TRUE(ValidateRHICapabilities(capabilities));

        capabilities.supportsTimestampQueries = false;
        EXPECT_FALSE(ValidateRHICapabilities(capabilities));
        EXPECT_NE(ValidateRHICapabilities(capabilities).message.find("Graphics timestamp frequency"),
                  std::string::npos);
    }

    TEST(RHIContractValidation,
         StagingCommitDefaultsToLegacyUnmapForCompatibleBackends)
    {
        LegacyStagingBuffer staging;
        ASSERT_NE(staging.Map(), nullptr);
        EXPECT_TRUE(staging.CommitMappedWrite());
        EXPECT_EQ(staging.m_mapCount, 1U);
        EXPECT_EQ(staging.m_unmapCount, 1U);
    }

    TEST(RHIContractValidation,
         RangeMappedWriteRejectsInvalidAndStaleAccessThenSupportsCancelRetry)
    {
        LegacyMappedWriteBuffer buffer;

        EXPECT_FALSE(buffer.MapWriteRange(0, 0).IsValid());
        EXPECT_FALSE(buffer.MapWriteRange(15, 2).IsValid());
        EXPECT_FALSE(buffer.MapWriteRange(std::numeric_limits<uint64>::max(), 1).IsValid());
        EXPECT_EQ(buffer.m_mapCount, 0U);

        LegacyMappedWriteBuffer readbackBuffer(RHIMemoryType::Readback);
        EXPECT_FALSE(readbackBuffer.MapWriteRange(0, 4).IsValid());
        EXPECT_EQ(readbackBuffer.m_mapCount, 0U);

        auto access = buffer.MapWriteRange(4, 4);
        ASSERT_TRUE(access.IsValid());
        EXPECT_EQ(access.GetData(), buffer.m_storage.data() + 4);
        EXPECT_FALSE(buffer.MapWriteRange(8, 2).IsValid());

        auto currentAccess = std::move(access);
        const RHIHostWriteReceipt staleReceipt =
            buffer.CommitMappedWriteRange(std::move(access));
        EXPECT_FALSE(staleReceipt.IsPublished());

        const RHIHostWriteReceipt receipt =
            buffer.CommitMappedWriteRange(std::move(currentAccess));
        EXPECT_TRUE(receipt.IsPublished());
        EXPECT_EQ(receipt.synchronization,
                  RHIHostWriteSynchronization::Unavailable);
        EXPECT_EQ(receipt.cpuWriteOffset, 4U);
        EXPECT_EQ(receipt.cpuWriteSize, 4U);
        EXPECT_FALSE(receipt.HasSynchronizedRange());
        EXPECT_EQ(receipt.synchronizedOffset, 0U);
        EXPECT_EQ(receipt.synchronizedSize, 0U);
        EXPECT_EQ(buffer.m_unmapCount, 1U);

        const RHIHostWriteReceipt doubleCommitReceipt =
            buffer.CommitMappedWriteRange(std::move(currentAccess));
        EXPECT_FALSE(doubleCommitReceipt.IsPublished());

        auto cancelledAccess = buffer.MapWriteRange(8, 2);
        ASSERT_TRUE(cancelledAccess.IsValid());
        EXPECT_TRUE(buffer.CancelMappedWriteRange(std::move(cancelledAccess)));
        EXPECT_FALSE(buffer.CancelMappedWriteRange(std::move(cancelledAccess)));
        EXPECT_EQ(buffer.m_unmapCount, 2U);

        auto retryAccess = buffer.MapWriteRange(8, 2);
        ASSERT_TRUE(retryAccess.IsValid());
        const RHIHostWriteReceipt retryReceipt =
            buffer.CommitMappedWriteRange(std::move(retryAccess));
        EXPECT_TRUE(retryReceipt.IsPublished());
        EXPECT_EQ(buffer.m_mapCount, 3U);
        EXPECT_EQ(buffer.m_unmapCount, 3U);
    }

    TEST(RHIContractValidation,
         RangeMappedWriteAccessAbortsOnScopeExitAndInvalidatesDestroyedOwners)
    {
        LegacyMappedWriteBuffer scopeBuffer;
        {
            auto access = scopeBuffer.MapWriteRange(2, 4);
            ASSERT_TRUE(access.IsValid());
        }
        EXPECT_EQ(scopeBuffer.m_unmapCount, 1U);

        auto retryAccess = scopeBuffer.MapWriteRange(2, 4);
        ASSERT_TRUE(retryAccess.IsValid());
        EXPECT_TRUE(scopeBuffer.CancelMappedWriteRange(std::move(retryAccess)));
        EXPECT_EQ(scopeBuffer.m_unmapCount, 2U);

        auto owner = std::make_unique<LegacyMappedWriteBuffer>();
        auto orphanedAccess = owner->MapWriteRange(1, 2);
        ASSERT_TRUE(orphanedAccess.IsValid());
        owner.reset();
        EXPECT_FALSE(orphanedAccess.IsValid());
        EXPECT_EQ(orphanedAccess.GetData(), nullptr);
    }

    TEST(RHIContractValidation,
         FailedRangeCommitAbortsExactlyOnceThenSupportsRetry)
    {
        FailingLegacyMappedWriteBuffer buffer;
        auto failedAccess = buffer.MapWriteRange(2, 4);
        ASSERT_TRUE(failedAccess.IsValid());
        EXPECT_FALSE(buffer.CommitMappedWriteRange(
            std::move(failedAccess)).IsPublished());
        EXPECT_EQ(buffer.commitCount, 1U);
        EXPECT_EQ(buffer.m_unmapCount, 1U);

        buffer.failCommit = false;
        auto retryAccess = buffer.MapWriteRange(2, 4);
        ASSERT_TRUE(retryAccess.IsValid());
        EXPECT_TRUE(buffer.CommitMappedWriteRange(
            std::move(retryAccess)).IsPublished());
        EXPECT_EQ(buffer.commitCount, 2U);
        EXPECT_EQ(buffer.m_unmapCount, 2U);
    }

    TEST(RHIContractValidation,
         FailedRangeAbortPoisonsOwnerInsteadOfPretendingCleanupSucceeded)
    {
        PoisonedCancelMappedWriteBuffer buffer;
        auto access = buffer.MapWriteRange(1, 2);
        ASSERT_TRUE(access.IsValid());
        EXPECT_FALSE(buffer.CancelMappedWriteRange(std::move(access)));
        EXPECT_EQ(buffer.cancelCount, 1U);
        EXPECT_FALSE(buffer.MapWriteRange(4, 2).IsValid());
    }

    TEST(RHIContractValidation,
         RangeMappedWriteControlPreventsWrongOwnerAndPlacementNewReuse)
    {
        LegacyMappedWriteBuffer owner;
        LegacyMappedWriteBuffer wrongOwner;
        auto access = owner.MapWriteRange(3, 2);
        ASSERT_TRUE(access.IsValid());
        const RHIHostWriteReceipt wrongOwnerReceipt =
            wrongOwner.CommitMappedWriteRange(std::move(access));
        EXPECT_FALSE(wrongOwnerReceipt.IsPublished());
        EXPECT_TRUE(access.IsValid());
        EXPECT_TRUE(owner.CancelMappedWriteRange(std::move(access)));

        using BufferStorage = std::aligned_storage_t<sizeof(LegacyMappedWriteBuffer),
                                                      alignof(LegacyMappedWriteBuffer)>;
        BufferStorage storage;
        auto* firstOwner = new (&storage) LegacyMappedWriteBuffer();
        auto staleAccess = firstOwner->MapWriteRange(4, 2);
        ASSERT_TRUE(staleAccess.IsValid());
        firstOwner->~LegacyMappedWriteBuffer();

        auto* replacementOwner = new (&storage) LegacyMappedWriteBuffer();
        EXPECT_FALSE(staleAccess.IsValid());
        EXPECT_EQ(staleAccess.GetData(), nullptr);
        EXPECT_FALSE(replacementOwner->CommitMappedWriteRange(
            std::move(staleAccess)).IsPublished());

        auto replacementAccess = replacementOwner->MapWriteRange(4, 2);
        ASSERT_TRUE(replacementAccess.IsValid());
        EXPECT_TRUE(replacementOwner->CancelMappedWriteRange(
            std::move(replacementAccess)));
        replacementOwner->~LegacyMappedWriteBuffer();
    }

    TEST(RHIContractValidation, NativeSurfaceRequiresBackendSpecificHandles)
    {
        EXPECT_TRUE(MakeSurface(NativeSurfacePlatform::Win32).IsValidFor(RHIBackendType::DX11));
        EXPECT_TRUE(MakeSurface(NativeSurfacePlatform::Win32).IsValidFor(RHIBackendType::DX12));
        EXPECT_TRUE(MakeSurface(NativeSurfacePlatform::Win32).IsValidFor(RHIBackendType::Vulkan));
        EXPECT_TRUE(MakeSurface(NativeSurfacePlatform::X11).IsValidFor(RHIBackendType::Vulkan));
        EXPECT_TRUE(MakeSurface(NativeSurfacePlatform::Wayland).IsValidFor(RHIBackendType::Vulkan));
        EXPECT_TRUE(MakeSurface(NativeSurfacePlatform::Cocoa).IsValidFor(RHIBackendType::Metal));
        EXPECT_TRUE(MakeSurface(NativeSurfacePlatform::UIKit).IsValidFor(RHIBackendType::Metal));
        EXPECT_TRUE(MakeSurface(NativeSurfacePlatform::GLFW).IsValidFor(RHIBackendType::OpenGL));

        NativeSurfaceDesc surface = MakeSurface(NativeSurfacePlatform::Win32);
        surface.nativeWindow = 0;
        EXPECT_FALSE(surface.IsValidFor(RHIBackendType::DX12));

        surface = MakeSurface(NativeSurfacePlatform::X11);
        surface.backendWindow = 0;
        EXPECT_FALSE(surface.IsValidFor(RHIBackendType::Vulkan));

        surface = MakeSurface(NativeSurfacePlatform::Cocoa);
        surface.nativeLayer = 0;
        EXPECT_FALSE(surface.IsValidFor(RHIBackendType::Metal));

        surface = MakeSurface(NativeSurfacePlatform::GLFW);
        surface.backendWindow = 0;
        EXPECT_FALSE(surface.IsValidFor(RHIBackendType::OpenGL));
    }

    TEST(RHIContractValidation, DescriptorSetsRequireCompleteImmutableSnapshots)
    {
        RHIDescriptorSetLayoutDesc layoutDesc;
        layoutDesc.AddBinding(0, RHIBindingType::UniformBuffer, RHIShaderStage::All, 2);
        layoutDesc.AddBinding(1, RHIBindingType::ShaderResourceBuffer);
        DescriptorContractLayout layout(layoutDesc);
        DescriptorContractBuffer buffer;

        RHIDescriptorSetDesc incompleteDesc;
        incompleteDesc.SetLayout(&layout).BindBuffer(0, &buffer, 0, 64, 0);
        const RHIDescriptorValidationResult incomplete =
            ValidateRHIDescriptorSetDesc(incompleteDesc);
        EXPECT_FALSE(incomplete);
        EXPECT_EQ(incomplete.code, RHIDescriptorValidationCode::IncompleteSnapshot);
        EXPECT_EQ(incomplete.binding, 0u);
        EXPECT_EQ(incomplete.arrayElement, 1u);

        RHIDescriptorSetDesc completeDesc;
        completeDesc.SetLayout(&layout)
            .BindBuffer(1, &buffer)
            .BindBuffer(0, &buffer, 0, 64, 1)
            .BindBuffer(0, &buffer, 0, 64, 0);
        ASSERT_TRUE(ValidateRHIDescriptorSetDesc(completeDesc));

        DescriptorContractSet descriptorSet(completeDesc);
        ASSERT_TRUE(descriptorSet.IsReadyForBinding(&layout));
        ASSERT_EQ(descriptorSet.GetDescriptorSnapshot().size(), 3u);
        EXPECT_EQ(descriptorSet.GetDescriptorSnapshot()[0].binding, 0u);
        EXPECT_EQ(descriptorSet.GetDescriptorSnapshot()[0].arrayElement, 0u);
        EXPECT_EQ(descriptorSet.GetDescriptorSnapshot()[1].binding, 0u);
        EXPECT_EQ(descriptorSet.GetDescriptorSnapshot()[1].arrayElement, 1u);
        EXPECT_EQ(descriptorSet.GetDescriptorSnapshot()[2].binding, 1u);
        EXPECT_TRUE(descriptorSet.Update({}));
        EXPECT_FALSE(descriptorSet.Update({completeDesc.bindings.front()}));
        EXPECT_FALSE(descriptorSet.HasBeenBound());
        descriptorSet.MarkBound();
        EXPECT_TRUE(descriptorSet.HasBeenBound());
        EXPECT_FALSE(descriptorSet.Update({completeDesc.bindings.front()}));

        DescriptorContractLayout differentLayout(layoutDesc);
        EXPECT_FALSE(descriptorSet.IsReadyForBinding(&differentLayout));
    }

    TEST(RHIContractValidation, DescriptorDataVolatilityIsIndependentFromSnapshotCompleteness)
    {
        RHIDescriptorSetLayoutDesc immutableReadLayout;
        immutableReadLayout.AddBinding(
            0,
            RHIBindingType::ShaderResourceBuffer,
            RHIShaderStage::Compute,
            1,
            RHIResourceDataVolatility::Immutable);
        EXPECT_TRUE(ValidateRHIDescriptorSetLayoutDesc(immutableReadLayout));

        RHIDescriptorSetLayoutDesc stableWhileBoundReadLayout;
        stableWhileBoundReadLayout.AddBinding(
            0,
            RHIBindingType::ShaderResourceBuffer,
            RHIShaderStage::Vertex,
            1,
            RHIResourceDataVolatility::StableWhileBound);
        EXPECT_TRUE(ValidateRHIDescriptorSetLayoutDesc(
            stableWhileBoundReadLayout));

        RHIDescriptorSetLayoutDesc invalidWritableLayout;
        invalidWritableLayout.AddBinding(
            0,
            RHIBindingType::StorageBuffer,
            RHIShaderStage::Compute,
            1,
            RHIResourceDataVolatility::Immutable);
        const RHIDescriptorValidationResult validation =
            ValidateRHIDescriptorSetLayoutDesc(invalidWritableLayout);
        EXPECT_FALSE(validation);
        EXPECT_EQ(validation.code, RHIDescriptorValidationCode::InvalidLayout);

        RHIDescriptorSetLayoutDesc invalidStableWritableLayout;
        invalidStableWritableLayout.AddBinding(
            0,
            RHIBindingType::StorageBuffer,
            RHIShaderStage::Compute,
            1,
            RHIResourceDataVolatility::StableWhileBound);
        EXPECT_FALSE(ValidateRHIDescriptorSetLayoutDesc(
            invalidStableWritableLayout));

        RHICapabilities capabilities = MakeValidCapabilities(RHIBackendType::Vulkan);
        capabilities.supportsDynamicDescriptorOffsets = false;
        RHIDescriptorSetLayoutDesc dynamicLayout;
        dynamicLayout.AddDynamicBinding(0, RHIBindingType::DynamicUniformBuffer);
        EXPECT_FALSE(ValidateRHIDescriptorSetLayoutCapabilities(dynamicLayout, capabilities));

        capabilities.supportsDynamicDescriptorOffsets = true;
        capabilities.supportsRaytracing = false;
        RHIDescriptorSetLayoutDesc accelerationStructureLayout;
        accelerationStructureLayout.AddBinding(
            0,
            RHIBindingType::AccelerationStructure,
            RHIShaderStage::RayGeneration);
        EXPECT_FALSE(ValidateRHIDescriptorSetLayoutCapabilities(
            accelerationStructureLayout,
            capabilities));
    }

    TEST(RHIContractValidation, ScopedAccessSeparatesMemoryLayoutDomainAndDiscard)
    {
        const RHIAccessSnapshot write = MakeRHIAccessSnapshot(
            RHIResourceState::UnorderedAccess,
            RHIShaderStage::Compute,
            GPUQueueDomain::Compute,
            RHIContentValidity::Valid);
        RHIAccessSnapshot read = write;
        read.memoryAccess = RHIMemoryAccess::ShaderRead;

        const RHIDependencyKind dependency = ClassifyRHIDependency(write, read);
        EXPECT_TRUE(HasDependencyKind(dependency, RHIDependencyKind::Memory));
        EXPECT_FALSE(HasDependencyKind(dependency, RHIDependencyKind::Transition));
        EXPECT_FALSE(HasDependencyKind(dependency, RHIDependencyKind::Ownership));
        EXPECT_EQ(ProjectRHIResourceState(write), RHIResourceState::UnorderedAccess);
        EXPECT_EQ(ProjectRHIResourceState(read), RHIResourceState::UnorderedAccess);
        EXPECT_TRUE(HasDependencyKind(
            ClassifyRHIDependency(read, write),
            RHIDependencyKind::Memory));

        RHIAccessSnapshot graphicsRead = read;
        graphicsRead.domain = GPUQueueDomain::Graphics;
        const RHIDependencyKind ownership = ClassifyRHIDependency(
            read,
            graphicsRead,
            RHIDiscardIntent::Discard);
        EXPECT_TRUE(HasDependencyKind(ownership, RHIDependencyKind::Ownership));
        EXPECT_TRUE(HasDependencyKind(ownership, RHIDependencyKind::Discard));

        RHIBufferBarrier barrier = MakeRHIBufferBarrier(
            nullptr,
            read,
            graphicsRead,
            64,
            128,
            RHIDiscardIntent::Discard);
        EXPECT_EQ(barrier.accessBefore, read);
        EXPECT_EQ(barrier.discardIntent, RHIDiscardIntent::Discard);
        EXPECT_NE(DescribeRHIAccessSnapshot(barrier.accessBefore).find("domain=Compute"),
                  std::string::npos);
    }

    TEST(RHIContractValidation, DX12EnhancedBarrierDialectRequiresNativeSupport)
    {
        RHICapabilities capabilities = MakeValidCapabilities(RHIBackendType::DX12);
        capabilities.dx12.barrierDialect = DX12BarrierDialect::Enhanced;
        capabilities.dx12.supportsEnhancedBarriers = false;

        const RHICapabilityValidationResult unsupported =
            ValidateRHICapabilities(capabilities);
        EXPECT_FALSE(unsupported);
        EXPECT_NE(unsupported.message.find("enhanced barrier dialect"),
                  std::string::npos);

        capabilities.dx12.supportsEnhancedBarriers = true;
        capabilities.supportsBufferRangeBarriers = true;
        EXPECT_TRUE(ValidateRHICapabilities(capabilities));
        EXPECT_STREQ(GetDX12BarrierDialectName(capabilities.dx12.barrierDialect),
                     "Enhanced");
    }

    TEST(RHIContractValidation, BufferRangeBarrierCapabilityIsFailClosed)
    {
        RHICapabilities capabilities =
            MakeValidCapabilities(RHIBackendType::Vulkan);
        capabilities.supportsBufferRangeBarriers = true;
        capabilities.supportsExplicitResourceBarriers = false;
        RHICapabilityValidationResult result =
            ValidateRHICapabilities(capabilities);
        EXPECT_FALSE(result);
        EXPECT_NE(result.message.find(
                      "buffer range barriers require explicit resource barriers"),
                  std::string::npos);

        capabilities = MakeValidCapabilities(RHIBackendType::DX12);
        capabilities.supportsBufferRangeBarriers = true;
        result = ValidateRHICapabilities(capabilities);
        EXPECT_FALSE(result);
        EXPECT_NE(result.message.find(
                      "must match the selected barrier dialect"),
                  std::string::npos);
    }

    TEST(RHIContractValidation, DeviceRuntimeFaultContractIsOwnedAndStable)
    {
        RHIDeviceFault fault;
        EXPECT_EQ(fault.status, RHIDeviceRuntimeStatus::Ready);
        EXPECT_EQ(fault.operation, RHIDeviceFaultOperation::None);
        EXPECT_FALSE(fault.IsFailure());
        EXPECT_FALSE(fault.IsDeviceLost());

        fault.status = RHIDeviceRuntimeStatus::DeviceLost;
        fault.operation = RHIDeviceFaultOperation::Present;
        fault.backend = RHIBackendType::Vulkan;
        fault.nativeError = 17U;
        fault.sequence = 3U;
        fault.message = "owned fault evidence";
        EXPECT_TRUE(fault.IsFailure());
        EXPECT_TRUE(fault.IsDeviceLost());
        EXPECT_TRUE(IsDeclaredRHIDeviceRuntimeStatus(fault.status));
        EXPECT_TRUE(IsDeclaredRHIDeviceFaultOperation(fault.operation));
    }

    TEST(RHIContractValidation,
         SoftwareAdapterFallbackIsExplicitAndDefaultsOff)
    {
        const RHIDeviceDesc defaults;
        EXPECT_FALSE(defaults.allowSoftwareAdapter);
    }

    TEST(RHIContractValidation, NativeSurfaceRejectsInvalidValueFields)
    {
        NativeSurfaceDesc surface = MakeSurface(NativeSurfacePlatform::Win32);

        surface.width = 0;
        EXPECT_FALSE(surface.IsValidFor(RHIBackendType::DX12));
        surface = MakeSurface(NativeSurfacePlatform::Win32);
        surface.height = 0;
        EXPECT_FALSE(surface.IsValidFor(RHIBackendType::DX12));
        surface = MakeSurface(NativeSurfacePlatform::Win32);
        surface.generation = 0;
        EXPECT_FALSE(surface.IsValidFor(RHIBackendType::DX12));
        surface = MakeSurface(NativeSurfacePlatform::Win32);
        surface.contentScale = 0.0f;
        EXPECT_FALSE(surface.IsValidFor(RHIBackendType::DX12));
        surface.contentScale = -1.0f;
        EXPECT_FALSE(surface.IsValidFor(RHIBackendType::DX12));
        surface.contentScale = std::numeric_limits<float32>::infinity();
        EXPECT_FALSE(surface.IsValidFor(RHIBackendType::DX12));
        surface.contentScale = std::numeric_limits<float32>::quiet_NaN();
        EXPECT_FALSE(surface.IsValidFor(RHIBackendType::DX12));
    }

    TEST(RHIContractValidation, NativeSurfaceUpdateClassificationUsesProductionLifecycleRules)
    {
        NativeSurfaceDesc current = MakeSurface(NativeSurfacePlatform::Win32);
        current.generation = 42;
        NativeSurfaceDesc update = current;

        update.generation = 42;
        EXPECT_EQ(NativeSurfaceUpdateKind::Reject,
                  ClassifyNativeSurfaceUpdate(current, update));
        update.generation = 41;
        EXPECT_EQ(NativeSurfaceUpdateKind::Reject,
                  ClassifyNativeSurfaceUpdate(current, update));

        update = current;
        update.generation = 43;
        update.width = 1920;
        update.height = 1080;
        EXPECT_EQ(NativeSurfaceUpdateKind::Resize,
                  ClassifyNativeSurfaceUpdate(current, update));

        const auto expectReplacement = [&](NativeSurfaceDesc replacement) {
            replacement.generation = 43;
            replacement.width = 1920;
            EXPECT_EQ(NativeSurfaceUpdateKind::Replace,
                      ClassifyNativeSurfaceUpdate(current, replacement));
        };

        update = current;
        update.nativeWindow = 0x5000;
        expectReplacement(update);
        update = current;
        update.nativeDisplay = 0x5000;
        expectReplacement(update);
        update = current;
        update.nativeLayer = 0x5000;
        expectReplacement(update);
        update = current;
        update.backendWindow = 0x5000;
        expectReplacement(update);
        update = current;
        update.platform = NativeSurfacePlatform::GLFW;
        expectReplacement(update);
        update = current;
        update.preferredFormat = RHIFormat::RGBA8_UNORM;
        expectReplacement(update);
        update = current;
        update.contentScale = 2.0f;
        expectReplacement(update);
        update = current;
        update.vsync = false;
        expectReplacement(update);

        update = current;
        update.generation = 43;
        EXPECT_EQ(NativeSurfaceUpdateKind::Replace,
                  ClassifyNativeSurfaceUpdate(current, update));
    }

    TEST(RHIContractValidation, SwapChainDescriptionHasSingleSurfaceSourceOfTruth)
    {
        static_assert(std::is_same_v<decltype(RHISwapChainDesc::surface), NativeSurfaceDesc>);
        static_assert(!HasWindowHandleField<RHISwapChainDesc>);
        static_assert(!HasWidthField<RHISwapChainDesc>);
        static_assert(!HasHeightField<RHISwapChainDesc>);
        static_assert(!HasFormatField<RHISwapChainDesc>);
        static_assert(!HasVsyncField<RHISwapChainDesc>);
    }

    TEST(RHIContractValidation, SurfaceRebindPolicyDoesNotDisableVulkanReplacement)
    {
        const FakeRHIDevice vulkanDevice(
            MakeValidCapabilities(RHIBackendType::Vulkan));
        NativeSurfaceDesc current = MakeSurface(NativeSurfacePlatform::Win32);
        NativeSurfaceDesc replacement = current;
        replacement.backendWindow = 0x5000;
        replacement.generation++;

        EXPECT_TRUE(vulkanDevice.SupportsSurfaceRebind(current, replacement));
    }

    TEST(RHIContractValidation, IndexedIndirectExecutionValidatesZeroOneMaximumAndCountClamp)
    {
        RHICapabilities capabilities = MakeValidCapabilities(RHIBackendType::DX12);
        EnableIndexedIndirectExecution(capabilities);

        IndirectExecutionContractBuffer arguments(
            sizeof(IndirectDrawIndexedCommand) * 4,
            RHIBufferUsage::IndirectArgs);
        IndirectExecutionContractBuffer counter(
            sizeof(uint32), RHIBufferUsage::IndirectArgs);

        RHIIndexedIndirectExecutionDesc desc;
        desc.maxDrawCount = 0;
        EXPECT_TRUE(ValidateRHIIndexedIndirectExecutionDesc(capabilities, desc));

        desc.argumentBuffer = &arguments;
        desc.commandStride = sizeof(IndirectDrawIndexedCommand);

        desc.maxDrawCount = 1;
        EXPECT_TRUE(ValidateRHIIndexedIndirectExecutionDesc(capabilities, desc));
        EXPECT_EQ(1u, ResolveRHIIndexedIndirectExecutionDrawCount(desc, 99));

        desc.maxDrawCount = capabilities.indexedIndirectExecution.maxDrawCount;
        EXPECT_TRUE(ValidateRHIIndexedIndirectExecutionDesc(capabilities, desc));

        IndirectExecutionContractBuffer variableStrideArguments(
            sizeof(IndirectDrawIndexedCommand) + 3 * 24,
            RHIBufferUsage::IndirectArgs);
        desc.argumentBuffer = &variableStrideArguments;
        desc.commandStride = 24;
        EXPECT_TRUE(ValidateRHIIndexedIndirectExecutionDesc(capabilities, desc));
        desc.argumentBuffer = &arguments;
        desc.commandStride = sizeof(IndirectDrawIndexedCommand);

        desc.mode = RHIIndirectExecutionMode::CountBuffer;
        desc.countBuffer = &counter;
        EXPECT_TRUE(ValidateRHIIndexedIndirectExecutionDesc(capabilities, desc));
        EXPECT_EQ(4u, ResolveRHIIndexedIndirectExecutionDrawCount(desc, 9));
        EXPECT_EQ(3u, ResolveRHIIndexedIndirectExecutionDrawCount(desc, 3));
        desc.mode = static_cast<RHIIndirectExecutionMode>(0xFFU);
        EXPECT_EQ(0u, ResolveRHIIndexedIndirectExecutionDrawCount(desc, 3));
        EXPECT_EQ(RHIIndexedIndirectExecutionValidationCode::InvalidMode,
                  ValidateRHIIndexedIndirectExecutionDesc(capabilities, desc).code);
        desc.mode = RHIIndirectExecutionMode::CountBuffer;

        ++desc.maxDrawCount;
        const RHIIndexedIndirectExecutionValidationResult overMaximum =
            ValidateRHIIndexedIndirectExecutionDesc(capabilities, desc);
        EXPECT_EQ(RHIIndexedIndirectExecutionValidationCode::DrawCountExceedsCapability,
                  overMaximum.code);
    }

    TEST(RHIContractValidation, IndexedIndirectExecutionRejectsContractViolationsSafely)
    {
        RHICapabilities capabilities = MakeValidCapabilities(RHIBackendType::DX12);
        EnableIndexedIndirectExecution(capabilities);
        IndirectExecutionContractBuffer arguments(
            sizeof(IndirectDrawIndexedCommand) * 2,
            RHIBufferUsage::IndirectArgs);
        IndirectExecutionContractBuffer counter(
            sizeof(uint32), RHIBufferUsage::IndirectArgs);
        IndirectExecutionContractBuffer wrongUsage(
            sizeof(IndirectDrawIndexedCommand) * 2,
            RHIBufferUsage::ShaderResource);
        IndirectExecutionContractBuffer wrongCounter(
            sizeof(uint32), RHIBufferUsage::ShaderResource);

        RHIIndexedIndirectExecutionDesc desc;
        desc.argumentBuffer = &arguments;
        desc.commandStride = sizeof(IndirectDrawIndexedCommand);
        desc.maxDrawCount = 1;

        desc.requiresFirstInstance = true;
        capabilities.indexedIndirectExecution.supportsFirstInstance = false;
        EXPECT_EQ(RHIIndexedIndirectExecutionValidationCode::FirstInstanceUnsupported,
                  ValidateRHIIndexedIndirectExecutionDesc(capabilities, desc).code);
        capabilities.indexedIndirectExecution.supportsFirstInstance = true;
        desc.requiresFirstInstance = false;
        capabilities.indexedIndirectExecution.supportsFixedCount = false;
        EXPECT_EQ(RHIIndexedIndirectExecutionValidationCode::CapabilityUnsupported,
                  ValidateRHIIndexedIndirectExecutionDesc(capabilities, desc).code);
        capabilities.indexedIndirectExecution.supportsFixedCount = true;
        desc.commandStride = sizeof(IndirectDrawIndexedCommand) - 4;
        EXPECT_EQ(RHIIndexedIndirectExecutionValidationCode::CommandStrideTooSmall,
                  ValidateRHIIndexedIndirectExecutionDesc(capabilities, desc).code);
        desc.commandStride = sizeof(IndirectDrawIndexedCommand);
        desc.argumentBuffer = nullptr;
        EXPECT_EQ(RHIIndexedIndirectExecutionValidationCode::MissingArgumentBuffer,
                  ValidateRHIIndexedIndirectExecutionDesc(capabilities, desc).code);
        desc.argumentBuffer = &arguments;

        desc.argumentOffset = 2;
        EXPECT_EQ(RHIIndexedIndirectExecutionValidationCode::ArgumentOffsetMisaligned,
                  ValidateRHIIndexedIndirectExecutionDesc(capabilities, desc).code);
        desc.argumentOffset = 0;
        desc.commandStride = sizeof(IndirectDrawIndexedCommand) + 2;
        EXPECT_EQ(RHIIndexedIndirectExecutionValidationCode::CommandStrideMisaligned,
                  ValidateRHIIndexedIndirectExecutionDesc(capabilities, desc).code);
        desc.commandStride = sizeof(IndirectDrawIndexedCommand);
        capabilities.indexedIndirectExecution.requiresExactCommandStride = true;
        desc.commandStride += 4;
        EXPECT_EQ(RHIIndexedIndirectExecutionValidationCode::CommandStrideMustMatchCommandSize,
                  ValidateRHIIndexedIndirectExecutionDesc(capabilities, desc).code);
        capabilities.indexedIndirectExecution.requiresExactCommandStride = false;
        desc.commandStride = sizeof(IndirectDrawIndexedCommand);
        desc.argumentState = RHIResourceState::ShaderResource;
        EXPECT_EQ(RHIIndexedIndirectExecutionValidationCode::ArgumentStateInvalid,
                  ValidateRHIIndexedIndirectExecutionDesc(capabilities, desc).code);
        desc.argumentState = RHIResourceState::IndirectArgument;
        desc.argumentBuffer = &wrongUsage;
        EXPECT_EQ(RHIIndexedIndirectExecutionValidationCode::ArgumentBufferUsageMissing,
                  ValidateRHIIndexedIndirectExecutionDesc(capabilities, desc).code);
        desc.argumentBuffer = &arguments;
        desc.maxDrawCount = 2;
        desc.argumentOffset = sizeof(IndirectDrawIndexedCommand);
        EXPECT_EQ(RHIIndexedIndirectExecutionValidationCode::ArgumentRangeOutOfBounds,
                  ValidateRHIIndexedIndirectExecutionDesc(capabilities, desc).code);
        desc.argumentOffset = std::numeric_limits<uint64>::max() - 3;
        desc.maxDrawCount = 1;
        EXPECT_EQ(RHIIndexedIndirectExecutionValidationCode::ArgumentRangeOverflow,
                  ValidateRHIIndexedIndirectExecutionDesc(capabilities, desc).code);

        desc.argumentOffset = 0;
        desc.mode = RHIIndirectExecutionMode::FixedCount;
        desc.countBuffer = &counter;
        EXPECT_EQ(RHIIndexedIndirectExecutionValidationCode::FixedCountHasCountBuffer,
                  ValidateRHIIndexedIndirectExecutionDesc(capabilities, desc).code);
        desc.mode = RHIIndirectExecutionMode::CountBuffer;
        desc.countBuffer = nullptr;
        EXPECT_EQ(RHIIndexedIndirectExecutionValidationCode::MissingCountBuffer,
                  ValidateRHIIndexedIndirectExecutionDesc(capabilities, desc).code);
        desc.countBuffer = &counter;
        desc.countState = RHIResourceState::ShaderResource;
        EXPECT_EQ(RHIIndexedIndirectExecutionValidationCode::CountStateInvalid,
                  ValidateRHIIndexedIndirectExecutionDesc(capabilities, desc).code);
        desc.countState = RHIResourceState::IndirectArgument;
        desc.countOffset = 2;
        EXPECT_EQ(RHIIndexedIndirectExecutionValidationCode::CountOffsetMisaligned,
                  ValidateRHIIndexedIndirectExecutionDesc(capabilities, desc).code);
        desc.countOffset = 0;
        desc.countBuffer = &wrongCounter;
        EXPECT_EQ(RHIIndexedIndirectExecutionValidationCode::CountBufferUsageMissing,
                  ValidateRHIIndexedIndirectExecutionDesc(capabilities, desc).code);
        desc.countBuffer = &counter;
        desc.countOffset = sizeof(uint32);
        EXPECT_EQ(RHIIndexedIndirectExecutionValidationCode::CountRangeOutOfBounds,
                  ValidateRHIIndexedIndirectExecutionDesc(capabilities, desc).code);
        desc.countOffset = 0;
        capabilities.indexedIndirectExecution.countValueSize = sizeof(uint16);
        EXPECT_EQ(RHIIndexedIndirectExecutionValidationCode::CountValueSizeInvalid,
                  ValidateRHIIndexedIndirectExecutionDesc(capabilities, desc).code);
        capabilities.indexedIndirectExecution.countValueSize = sizeof(uint32);
        desc.countOffset = std::numeric_limits<uint64>::max() - 3;
        EXPECT_EQ(RHIIndexedIndirectExecutionValidationCode::CountRangeOverflow,
                  ValidateRHIIndexedIndirectExecutionDesc(capabilities, desc).code);

        desc.countOffset = 0;
        desc.mode = RHIIndirectExecutionMode::FixedCount;
        desc.countBuffer = nullptr;
        capabilities.indexedIndirectExecution.requiresExactCommandStride = true;
        capabilities.indexedIndirectExecution.minCommandStride =
            sizeof(IndirectDrawIndexedCommand) + 4;
        EXPECT_FALSE(ValidateRHICapabilities(capabilities));
        EXPECT_NE(ValidateRHICapabilities(capabilities).message.find("exact indexed indirect command stride"),
                  std::string::npos);
        capabilities.indexedIndirectExecution.requiresExactCommandStride = false;
        capabilities.indexedIndirectExecution.minCommandStride =
            sizeof(IndirectDrawIndexedCommand);

        capabilities.supportsIndirectDrawCount = false;
        EXPECT_FALSE(ValidateRHICapabilities(capabilities));
        EXPECT_NE(ValidateRHICapabilities(capabilities).message.find("supportsIndirectDrawCount"),
                  std::string::npos);
    }

    TEST(RHIContractValidation, AcceptsMinimalConcreteBackendCapabilities)
    {
        RHICapabilities capabilities = MakeValidCapabilities(RHIBackendType::DX12);

        auto result = ValidateRHICapabilities(capabilities);

        EXPECT_TRUE(result) << result.message;
        EXPECT_TRUE(result.message.empty());
    }

    TEST(RHIContractValidation, RejectsNonConcreteBackendIdentity)
    {
        RHICapabilities capabilities = MakeValidCapabilities(RHIBackendType::DX12);
        capabilities.backendType = RHIBackendType::Auto;

        auto result = ValidateRHICapabilities(capabilities);

        EXPECT_FALSE(result);
        EXPECT_NE(result.message.find("concrete backend"), std::string::npos);
    }

    TEST(RHIContractValidation, RejectsDescriptorSetContradictions)
    {
        RHICapabilities capabilities = MakeValidCapabilities(RHIBackendType::DX12);
        capabilities.maxDescriptorSets = 0;

        auto result = ValidateRHICapabilities(capabilities);

        EXPECT_FALSE(result);
        EXPECT_NE(result.message.find("maxDescriptorSets"), std::string::npos);

        capabilities = MakeValidCapabilities(RHIBackendType::DX12);
        capabilities.supportsDescriptorSets = false;

        result = ValidateRHICapabilities(capabilities);

        EXPECT_FALSE(result);
        EXPECT_NE(result.message.find("maxDescriptorSets"), std::string::npos);

        capabilities = MakeValidCapabilities(RHIBackendType::DX12);
        capabilities.supportsDescriptorSets = false;
        capabilities.supportsDynamicDescriptorOffsets = true;
        capabilities.maxDescriptorSets = 0;

        result = ValidateRHICapabilities(capabilities);

        EXPECT_FALSE(result);
        EXPECT_NE(result.message.find("dynamic descriptor offsets"), std::string::npos);
    }

    TEST(RHIContractValidation, RejectsBarrierContradictions)
    {
        RHICapabilities capabilities = MakeValidCapabilities(RHIBackendType::DX12);
        capabilities.emulatesResourceBarriers = true;

        auto result = ValidateRHICapabilities(capabilities);

        EXPECT_FALSE(result);
        EXPECT_NE(result.message.find("both explicit and emulated"), std::string::npos);

        capabilities = MakeValidCapabilities(RHIBackendType::DX11);
        capabilities.supportsExplicitResourceBarriers = false;
        capabilities.supportsSplitBarrier = true;

        result = ValidateRHICapabilities(capabilities);

        EXPECT_FALSE(result);
        EXPECT_NE(result.message.find("split barriers"), std::string::npos);

        capabilities = MakeValidCapabilities(RHIBackendType::DX11);
        capabilities.supportsExplicitResourceBarriers = false;
        capabilities.supportsExplicitAliasingBarriers = true;

        result = ValidateRHICapabilities(capabilities);

        EXPECT_FALSE(result);
        EXPECT_NE(result.message.find("aliasing barriers"), std::string::npos);
    }

    TEST(RHIContractValidation, RejectsQueueFenceContradictions)
    {
        RHICapabilities capabilities = MakeValidCapabilities(RHIBackendType::DX12);
        capabilities.supportsExplicitQueueFenceSignal = false;
        capabilities.supportsQueueFenceWait = true;

        auto result = ValidateRHICapabilities(capabilities);

        EXPECT_FALSE(result);
        EXPECT_NE(result.message.find("queue fence waits"), std::string::npos);

        capabilities = MakeValidCapabilities(RHIBackendType::DX11);
        capabilities.supportsExplicitQueueFenceSignal = true;
        capabilities.supportsQueueFenceWait = true;
        capabilities.emulatesQueueFences = true;

        result = ValidateRHICapabilities(capabilities);

        EXPECT_FALSE(result);
        EXPECT_NE(result.message.find("emulated queue fences"), std::string::npos);

        capabilities = MakeValidCapabilities(RHIBackendType::DX12);
        capabilities.supportsQueueSubmissionPlan = true;
        EXPECT_TRUE(ValidateRHICapabilities(capabilities));

        capabilities.queueTopology.completionMode =
            RHIQueueCompletionMode::CompatibilityWaitIdle;
        result = ValidateRHICapabilities(capabilities);
        EXPECT_FALSE(result);
        EXPECT_NE(result.message.find("queue submission plans"), std::string::npos);
    }

    TEST(RHIContractValidation, RejectsAsyncComputeWithoutComputePipeline)
    {
        RHICapabilities capabilities = MakeValidCapabilities(RHIBackendType::DX12);
        capabilities.supportsComputePipeline = false;
        capabilities.supportsAsyncCompute = true;

        auto result = ValidateRHICapabilities(capabilities);

        EXPECT_FALSE(result);
        EXPECT_NE(result.message.find("compute pipeline"), std::string::npos);
    }

    TEST(RHIContractValidation, RejectsRayTracingPipelineWithoutRequiredLimits)
    {
        RHICapabilities capabilities = MakeValidCapabilities(RHIBackendType::DX12);
        capabilities.supportsRaytracing = true;
        capabilities.supportsRaytracingPipeline = true;

        auto result = ValidateRHICapabilities(capabilities);

        EXPECT_FALSE(result);
        EXPECT_NE(result.message.find("shader table limits"), std::string::npos);

        capabilities.maxRayRecursionDepth = 31;
        capabilities.shaderGroupHandleSize = 32;
        capabilities.shaderGroupHandleAlignment = 32;
        capabilities.shaderTableBaseAlignment = 64;

        result = ValidateRHICapabilities(capabilities);

        EXPECT_TRUE(result) << result.message;
    }

    TEST(RHIContractValidation, RejectsBackendSpecificMissingMetadata)
    {
        RHICapabilities vulkan = MakeValidCapabilities(RHIBackendType::Vulkan);
        vulkan.vulkan.apiVersion = 0;

        auto result = ValidateRHICapabilities(vulkan);

        EXPECT_FALSE(result);
        EXPECT_NE(result.message.find("apiVersion"), std::string::npos);

        RHICapabilities opengl = MakeValidCapabilities(RHIBackendType::OpenGL);
        opengl.opengl.majorVersion = 0;

        result = ValidateRHICapabilities(opengl);

        EXPECT_FALSE(result);
        EXPECT_NE(result.message.find("majorVersion"), std::string::npos);
    }

    TEST(RHIContractValidation, CapabilityReportClassifiesCoreBackendContracts)
    {
        RHICapabilities capabilities = MakeValidCapabilities(RHIBackendType::DX12);
        capabilities.supportsAsyncCompute = true;
        EnableIndexedIndirectExecution(capabilities);
        capabilities.supportsTimestampQueries = true;
        capabilities.timestampFrequency = 1000000000ull;
        capabilities.supportsMemoryBudgetQuery = true;
        capabilities.supportsExplicitHeapManagement = true;

        const RHICapabilityReport report = BuildRHICapabilityReport(capabilities);

        EXPECT_EQ(report.schemaVersion, RVX_RHI_CAPABILITY_REPORT_SCHEMA_VERSION);
        EXPECT_EQ(report.backendType, RHIBackendType::DX12);
        EXPECT_EQ(report.adapterName, capabilities.adapterName);
        EXPECT_EQ(report.driverVersion, capabilities.driverVersion);
        EXPECT_TRUE(report.validationPassed) << report.validationMessage;
        EXPECT_TRUE(report.renderGraphBaselineSupported);
        EXPECT_TRUE(report.renderGraphBaselineMissingRequirements.empty());
        EXPECT_EQ(report.entries.size(), static_cast<size_t>(12));
        EXPECT_STREQ(GetRHICapabilityFeatureName(RHICapabilityFeature::RayTracing), "RayTracing");
        EXPECT_STREQ(GetRHICapabilityStatusName(RHICapabilityStatus::Unsupported), "Unsupported");
        EXPECT_GE(report.supportedCount, 7u);
        EXPECT_EQ(report.emulatedCount, 0u);
        EXPECT_GE(report.unsupportedCount, 1u);

        const RHICapabilityReportEntry* compute =
            FindReportEntry(report, RHICapabilityFeature::ComputePipeline);
        ASSERT_NE(compute, nullptr);
        EXPECT_EQ(compute->status, RHICapabilityStatus::Supported);
        EXPECT_TRUE(compute->supported);
        EXPECT_FALSE(compute->emulated);
        EXPECT_EQ(compute->requiredCapability, "supportsComputePipeline");

        const RHICapabilityReportEntry* rayTracing =
            FindReportEntry(report, RHICapabilityFeature::RayTracing);
        ASSERT_NE(rayTracing, nullptr);
        EXPECT_EQ(rayTracing->status, RHICapabilityStatus::Unsupported);
        EXPECT_FALSE(rayTracing->supported);
        EXPECT_EQ(rayTracing->requiredCapability, "supportsRaytracing+supportsRaytracingPipeline");
        EXPECT_NE(rayTracing->diagnosticMessage.find("unavailable"), std::string::npos);

        const RHICapabilityReportEntry* indexedIndirect =
            FindReportEntry(report, RHICapabilityFeature::IndexedIndirectExecution);
        ASSERT_NE(indexedIndirect, nullptr);
        EXPECT_EQ(indexedIndirect->status, RHICapabilityStatus::Supported);
        EXPECT_TRUE(report.indexedIndirectExecution.supportsCountBuffer);

        const RHICapabilityReportEntry* legacyCount =
            FindReportEntry(report, RHICapabilityFeature::IndirectDrawCount);
        ASSERT_NE(legacyCount, nullptr);
        EXPECT_EQ(legacyCount->status, RHICapabilityStatus::Supported);
        EXPECT_EQ(legacyCount->requiredCapability,
                  "indexedIndirectExecution.supportsCountBuffer");
    }

    TEST(RHIContractValidation, CapabilityReportSurfacesEmulationAndValidationFailures)
    {
        RHICapabilities capabilities = MakeValidCapabilities(RHIBackendType::DX11);
        capabilities.supportsExplicitResourceBarriers = false;
        capabilities.emulatesResourceBarriers = true;
        capabilities.supportsDefaultQueueFenceSignal = true;
        capabilities.emulatesQueueFences = true;

        RHICapabilityReport report = BuildRHICapabilityReport(capabilities);
        EXPECT_TRUE(report.validationPassed) << report.validationMessage;
        EXPECT_GE(report.emulatedCount, 2u);

        const RHICapabilityReportEntry* barriers =
            FindReportEntry(report, RHICapabilityFeature::ExplicitResourceBarriers);
        ASSERT_NE(barriers, nullptr);
        EXPECT_EQ(barriers->status, RHICapabilityStatus::Emulated);
        EXPECT_TRUE(barriers->supported);
        EXPECT_TRUE(barriers->emulated);
        EXPECT_NE(barriers->diagnosticMessage.find("emulated"), std::string::npos);

        capabilities.supportsExplicitResourceBarriers = true;
        report = BuildRHICapabilityReport(capabilities);

        EXPECT_FALSE(report.validationPassed);
        EXPECT_FALSE(report.renderGraphBaselineSupported);
        EXPECT_TRUE(HasMissingRequirement(report, "ValidateRHICapabilities"));
        EXPECT_NE(report.validationMessage.find("both explicit and emulated"), std::string::npos);
    }

    TEST(RHIContractValidation, CapabilityReportExposesRenderGraphBaselineReadiness)
    {
        RHICapabilities capabilities = MakeValidCapabilities(RHIBackendType::DX12);

        RHICapabilityReport report = BuildRHICapabilityReport(capabilities);

        EXPECT_TRUE(report.validationPassed) << report.validationMessage;
        EXPECT_TRUE(report.renderGraphBaselineSupported);
        EXPECT_TRUE(report.renderGraphBaselineMissingRequirements.empty());

        capabilities.supportsDescriptorSets = false;
        capabilities.supportsDynamicDescriptorOffsets = false;
        capabilities.maxDescriptorSets = 0;

        report = BuildRHICapabilityReport(capabilities);

        EXPECT_TRUE(report.validationPassed) << report.validationMessage;
        EXPECT_FALSE(report.renderGraphBaselineSupported);
        EXPECT_TRUE(HasMissingRequirement(report, "supportsDescriptorSets+maxDescriptorSets"));
        EXPECT_FALSE(HasMissingRequirement(report, "ValidateRHICapabilities"));
    }

    TEST(RHIContractValidation, DeviceCapabilityReportUsesPublicDeviceCapabilities)
    {
        RHICapabilities capabilities = MakeValidCapabilities(RHIBackendType::Vulkan);
        capabilities.supportsAsyncCompute = true;
        capabilities.queueTopology.logicalQueueDomains[1] = GPUQueueDomain::Compute;
        capabilities.queueTopology.activeDomainCount = 2;
        EnableIndexedIndirectExecution(capabilities);
        capabilities.supportsTimestampQueries = true;
        capabilities.timestampFrequency = 1000000000ull;

        const FakeRHIDevice device(capabilities);
        const RHICapabilityReport report = device.GetCapabilityReport();

        EXPECT_EQ(report.schemaVersion, RVX_RHI_CAPABILITY_REPORT_SCHEMA_VERSION);
        EXPECT_EQ(report.backendType, RHIBackendType::Vulkan);
        EXPECT_EQ(report.adapterName, capabilities.adapterName);
        EXPECT_EQ(report.driverVersion, capabilities.driverVersion);
        EXPECT_TRUE(report.validationPassed) << report.validationMessage;
        EXPECT_EQ(report.entries.size(), static_cast<size_t>(12));

        const RHICapabilityReportEntry* asyncCompute =
            FindReportEntry(report, RHICapabilityFeature::AsyncCompute);
        ASSERT_NE(asyncCompute, nullptr);
        EXPECT_EQ(asyncCompute->status, RHICapabilityStatus::Supported);
        EXPECT_EQ(asyncCompute->requiredCapability, "supportsAsyncCompute+supportsComputePipeline");
    }

    TEST(RHIContractValidation, CapabilityReportTextExportIsStableForTools)
    {
        RHICapabilities capabilities = MakeValidCapabilities(RHIBackendType::OpenGL);
        capabilities.supportsExplicitResourceBarriers = false;
        capabilities.emulatesResourceBarriers = true;
        capabilities.supportsDefaultQueueFenceSignal = false;
        capabilities.emulatesQueueFences = true;

        const FakeRHIDevice device(capabilities);
        const std::string text = device.ExportCapabilityReportText();

        EXPECT_NE(text.find("RHI Capability Report"), std::string::npos);
        EXPECT_NE(text.find("Schema: 6"), std::string::npos);
        EXPECT_NE(text.find("Backend: OpenGL"), std::string::npos);
        EXPECT_NE(text.find("Adapter: OpenGL Test Adapter"), std::string::npos);
        EXPECT_NE(text.find("DriverVersion: TestDriver.1"), std::string::npos);
        EXPECT_NE(text.find("Validation: Passed"), std::string::npos);
        EXPECT_NE(text.find("QueueCompletionMode: CompatibilityWaitIdle"), std::string::npos);
        EXPECT_NE(text.find("QueueDomains: Graphics=Graphics, Compute=Graphics, Copy=Graphics, Active=1"),
                  std::string::npos);
        EXPECT_NE(text.find("RenderGraphBaseline: Passed"), std::string::npos);
        EXPECT_NE(text.find("RenderGraphBaselineMissing: none"), std::string::npos);
        EXPECT_NE(text.find("ExplicitResourceBarriers: Emulated"), std::string::npos);
        EXPECT_NE(text.find("QueueSynchronization: Emulated"), std::string::npos);
        EXPECT_NE(text.find("Summary: supported="), std::string::npos);
    }

    TEST(RHIContractValidation, CapabilityReportJsonExportIsStableForTools)
    {
        RHICapabilities capabilities = MakeValidCapabilities(RHIBackendType::OpenGL);
        capabilities.supportsExplicitResourceBarriers = false;
        capabilities.emulatesResourceBarriers = true;
        capabilities.supportsDefaultQueueFenceSignal = false;
        capabilities.emulatesQueueFences = true;

        const FakeRHIDevice device(capabilities);
        const std::string json = device.ExportCapabilityReportJson();

        EXPECT_NE(json.find("\"schemaVersion\": 6"), std::string::npos);
        EXPECT_NE(json.find("\"schemaId\": \"RVX.RHI.CapabilityReport\""), std::string::npos);
        EXPECT_NE(json.find("\"id\": \"rhiCapabilityReportJson\""), std::string::npos);
        EXPECT_NE(json.find("\"kind\": \"RHICapabilityReportJson\""), std::string::npos);
        EXPECT_NE(json.find("\"contentType\": \"application/json\""), std::string::npos);
        EXPECT_NE(json.find("\"contentHash\": \"\""), std::string::npos);
        EXPECT_NE(json.find("\"relativePath\": \"\""), std::string::npos);
        EXPECT_NE(json.find("\"backend\": \"OpenGL\""), std::string::npos);
        EXPECT_NE(json.find("\"adapterName\": \"OpenGL Test Adapter\""), std::string::npos);
        EXPECT_NE(json.find("\"driverVersion\": \"TestDriver.1\""), std::string::npos);
        EXPECT_NE(json.find("\"validationPassed\": true"), std::string::npos);
        EXPECT_NE(json.find("\"completionMode\": \"CompatibilityWaitIdle\""), std::string::npos);
        EXPECT_NE(json.find("\"logicalQueueDomains\": [\"Graphics\", \"Graphics\", \"Graphics\"]"),
                  std::string::npos);
        EXPECT_NE(json.find("\"renderGraphBaseline\": {"), std::string::npos);
        EXPECT_NE(json.find("\"indexedIndirectExecution\": {"), std::string::npos);
        EXPECT_NE(json.find("\"supported\": true"), std::string::npos);
        EXPECT_NE(json.find("\"missingRequirements\": []"), std::string::npos);
        EXPECT_NE(json.find("\"feature\": \"ExplicitResourceBarriers\""), std::string::npos);
        EXPECT_NE(json.find("\"status\": \"Emulated\""), std::string::npos);
    }

    TEST(RHIContractValidation, OptimizedClearIsOptionalTypedAndPartOfTextureIdentity)
    {
        RHITextureDesc absent = RHITextureDesc::RenderTarget(
            64, 64, RHIFormat::RGBA16_FLOAT);
        EXPECT_TRUE(IsRHIOptimizedClearValueCompatible(absent));
        EXPECT_FALSE(absent.optimizedClearValue.IsPresent());

        RHITextureDesc color = absent;
        color.SetOptimizedClearColor({0.1f, 0.2f, 0.3f, 1.0f});
        EXPECT_TRUE(IsRHIOptimizedClearValueCompatible(color));
        EXPECT_FALSE(AreRHITextureDescsEquivalent(absent, color));

        RHITextureDesc sameColor = color;
        EXPECT_TRUE(AreRHITextureDescsEquivalent(color, sameColor));
        sameColor.optimizedClearValue.color.b = 0.4f;
        EXPECT_FALSE(AreRHITextureDescsEquivalent(color, sameColor));

        RHITextureDesc depth = RHITextureDesc::DepthStencil(
            64, 64, RHIFormat::D32_FLOAT);
        depth.SetOptimizedClearDepthStencil({0.0f, 0});
        EXPECT_TRUE(IsRHIOptimizedClearValueCompatible(depth));

        depth.SetOptimizedClearColor({0.0f, 0.0f, 0.0f, 1.0f});
        EXPECT_FALSE(IsRHIOptimizedClearValueCompatible(depth));

        color.SetOptimizedClearDepthStencil({1.0f, 0});
        EXPECT_FALSE(IsRHIOptimizedClearValueCompatible(color));
    }

    TEST(RHIContractValidation, TextureViewFormatsFailClosedBeforeBackendCreation)
    {
        RHITextureViewDesc desc;
        desc.type = RHITextureViewType::ShaderResource;
        EXPECT_TRUE(IsTextureViewTypeCompatible(
            RHITextureUsage::ShaderResource,
            RHIFormat::RGBA16_FLOAT,
            desc));

        desc.format = RHIFormat::Count;
        EXPECT_FALSE(IsTextureViewTypeCompatible(
            RHITextureUsage::ShaderResource,
            RHIFormat::RGBA16_FLOAT,
            desc));

        desc.format = static_cast<RHIFormat>(0xDD);
        EXPECT_FALSE(IsTextureViewTypeCompatible(
            RHITextureUsage::ShaderResource,
            RHIFormat::RGBA16_FLOAT,
            desc));

        desc.format = RHIFormat::Unknown;
        EXPECT_FALSE(IsTextureViewTypeCompatible(
            RHITextureUsage::ShaderResource,
            RHIFormat::Unknown,
            desc));
    }

    TEST(RHIContractValidation, QueueSubmissionPlanRequiresTopologicalTerminalJoin)
    {
        QueueContractContext copy(RHICommandQueueType::Copy);
        QueueContractContext compute(RHICommandQueueType::Compute);
        QueueContractContext graphics(RHICommandQueueType::Graphics);

        RHIQueueSubmissionPlan plan;
        plan.batches = {
            {RHICommandQueueType::Copy, {&copy}, {}},
            {RHICommandQueueType::Compute, {&compute}, {0}},
            {RHICommandQueueType::Graphics, {&graphics}, {1}},
        };
        plan.terminalGraphicsBatchIndex = 2;
        EXPECT_TRUE(ValidateRHIQueueSubmissionPlan(plan));

        plan.batches[2].prerequisiteBatchIndices = {1, 0};
        EXPECT_TRUE(ValidateRHIQueueSubmissionPlan(plan));

        plan.batches[2].prerequisiteBatchIndices = {1};
        plan.batches[1].prerequisiteBatchIndices.clear();
        const auto disconnected = ValidateRHIQueueSubmissionPlan(plan);
        EXPECT_FALSE(disconnected);
        EXPECT_NE(disconnected.message.find("does not join"), std::string::npos);
    }

    TEST(RHIContractValidation, QueueSubmissionPlanRejectsInvalidOwnershipAndOrdering)
    {
        QueueContractContext compute(RHICommandQueueType::Compute);
        QueueContractContext graphics(RHICommandQueueType::Graphics);

        RHIQueueSubmissionPlan plan;
        plan.batches = {
            {RHICommandQueueType::Compute, {&compute}, {}},
            {RHICommandQueueType::Graphics, {&graphics}, {0}},
        };
        plan.terminalGraphicsBatchIndex = 1;
        ASSERT_TRUE(ValidateRHIQueueSubmissionPlan(plan));

        plan.batches[0].queueType = RHICommandQueueType::Graphics;
        EXPECT_FALSE(ValidateRHIQueueSubmissionPlan(plan));
        plan.batches[0].queueType = RHICommandQueueType::Compute;

        plan.batches[1].prerequisiteBatchIndices = {1};
        EXPECT_FALSE(ValidateRHIQueueSubmissionPlan(plan));
        plan.batches[1].prerequisiteBatchIndices = {0};

        plan.batches[1].contexts = {&compute};
        EXPECT_FALSE(ValidateRHIQueueSubmissionPlan(plan));
        plan.batches[1].contexts = {&graphics};

        plan.batches[0].queueType = static_cast<RHICommandQueueType>(0xFF);
        EXPECT_FALSE(ValidateRHIQueueSubmissionPlan(plan));
    }

} // namespace RVX::Tests
