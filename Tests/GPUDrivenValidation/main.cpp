#include "Core/Log.h"
#include "Render/GPUScene/GPUSceneSchema.h"
#include "GPUScene/GPUSceneUploader.h"
#include "Render/GPUDriven/GPUCulling.h"
#include "Render/GPUDriven/GPUDrivenDiagnostics.h"
#include "Render/GPUDriven/GPUDrivenPolicy.h"
#include "Render/Renderer/RenderDrawPacket.h"
#include "Render/Renderer/RenderDrawItem.h"
#include "Render/Renderer/RenderScene.h"
#include "Render/Submission/RenderSubmissionStrategy.h"
#include "Render/Visibility/RenderVisibility.h"
#include "RenderContracts/RenderFramePacket.h"
#include "Resources/RenderRetirementQueue.h"
#include "Resources/RenderSubmissionResourceBatch.h"
#include "Resources/RenderSubmissionTracker.h"
#include "RHI/RHI.h"
#include "RHI/RHICommandContext.h"
#include "ShaderCompiler/ShaderCompiler.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <string>
#include <vector>

using namespace RVX;

namespace
{
    struct BufferLifetimeState
    {
        uint32 destroyedCount = 0;
    };

    class FakeBuffer final : public RHIBuffer
    {
    public:
        explicit FakeBuffer(const RHIBufferDesc& desc,
                            std::shared_ptr<BufferLifetimeState> lifetimeState = {})
            : m_desc(desc)
            , m_storage(static_cast<size_t>(desc.size))
            , m_lifetimeState(std::move(lifetimeState))
        {
            m_debugName = desc.debugName ? desc.debugName : "";
        }

        ~FakeBuffer() override
        {
            if (m_lifetimeState)
            {
                ++m_lifetimeState->destroyedCount;
            }
        }

        uint64 GetSize() const override { return m_desc.size; }
        RHIBufferUsage GetUsage() const override { return m_desc.usage; }
        RHIMemoryType GetMemoryType() const override { return m_desc.memoryType; }
        uint32 GetStride() const override { return m_desc.stride; }

        void* Map() override
        {
            return !m_mapSucceeds || m_storage.empty() ? nullptr : m_storage.data();
        }
        void Unmap() override {}

        const std::vector<uint8>& GetStorage() const { return m_storage; }
        void SetMapSucceeds(bool succeeds) { m_mapSucceeds = succeeds; }
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
        std::shared_ptr<BufferLifetimeState> m_lifetimeState;
        bool m_mapSucceeds = true;
    };

    class FakeShader final : public RHIShader
    {
    public:
        explicit FakeShader(const RHIShaderDesc& desc)
            : m_stage(desc.stage)
        {
            if (desc.bytecode != nullptr && desc.bytecodeSize != 0)
            {
                const auto* bytes = static_cast<const uint8*>(desc.bytecode);
                m_bytecode.assign(bytes, bytes + desc.bytecodeSize);
            }
        }

        RHIShaderStage GetStage() const override { return m_stage; }
        const std::vector<uint8>& GetBytecode() const override { return m_bytecode; }

    private:
        RHIShaderStage m_stage = RHIShaderStage::None;
        std::vector<uint8> m_bytecode;
    };

    class FakeDescriptorSetLayout final : public RHIDescriptorSetLayout
    {
    public:
        explicit FakeDescriptorSetLayout(const RHIDescriptorSetLayoutDesc& desc)
            : m_entries(desc.entries)
        {
        }

        const std::vector<RHIBindingLayoutEntry>& GetEntries() const override
        {
            return m_entries;
        }

    private:
        std::vector<RHIBindingLayoutEntry> m_entries;
    };

    class FakePipelineLayout final : public RHIPipelineLayout
    {
    public:
        explicit FakePipelineLayout(const RHIPipelineLayoutDesc& desc)
            : RHIPipelineLayout(desc)
        {
        }
    };

    class FakeComputePipeline final : public RHIPipeline
    {
    public:
        bool IsCompute() const override { return true; }
    };

    class FakeDescriptorSet final : public RHIDescriptorSet
    {
    public:
        explicit FakeDescriptorSet(const RHIDescriptorSetDesc& desc)
            : RHIDescriptorSet(desc)
        {
        }

        bool Update(const std::vector<RHIDescriptorBinding>&) override
        {
            return false;
        }
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
        void SignalOnQueue(uint64 value, RHICommandQueueType) override
        {
            Complete(value);
        }
        void Wait(uint64 value, uint64 = UINT64_MAX) override { Complete(value); }
        uint64 AllocateValue() { return m_nextValue++; }
        void Complete(uint64 value)
        {
            m_completedValue = std::max(m_completedValue, value);
        }

    private:
        uint64 m_completedValue = 0;
        uint64 m_nextValue = 1;
    };

    class FakeCommandContext final : public RHICommandContext
    {
    public:
        RHICommandQueueType GetQueueType() const override { return RHICommandQueueType::Graphics; }
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
        void Barriers(std::span<const RHIBufferBarrier>, std::span<const RHITextureBarrier>) override {}
        void BeginBarrier(const RHIBufferBarrier&) override {}
        void BeginBarrier(const RHITextureBarrier&) override {}
        void EndBarrier(const RHIBufferBarrier&) override {}
        void EndBarrier(const RHITextureBarrier&) override {}
        void BeginRenderPass(const RHIRenderPassDesc&) override {}
        void EndRenderPass() override {}
        void SetPipeline(RHIPipeline* pipeline) override
        {
            pipelines.push_back(pipeline);
        }
        void SetVertexBuffer(uint32, RHIBuffer*, uint64 = 0) override {}
        void SetVertexBuffers(uint32, std::span<RHIBuffer* const>, std::span<const uint64> = {}) override {}
        void SetIndexBuffer(RHIBuffer*, RHIFormat, uint64 = 0) override {}
        void SetDescriptorSet(uint32,
                              RHIDescriptorSet* descriptorSet,
                              std::span<const uint32> = {}) override
        {
            descriptorSets.push_back(descriptorSet);
        }
        void SetPushConstants(const void*, uint32, uint32 = 0) override {}
        void SetViewport(const RHIViewport&) override {}
        void SetViewports(std::span<const RHIViewport>) override {}
        void SetScissor(const RHIRect&) override {}
        void SetScissors(std::span<const RHIRect>) override {}
        void Draw(uint32, uint32 = 1, uint32 = 0, uint32 = 0) override {}
        void DrawIndexed(uint32 indexCount,
                         uint32 instanceCount = 1,
                         uint32 firstIndex = 0,
                         int32 vertexOffset = 0,
                         uint32 firstInstance = 0) override
        {
            ++drawIndexedCalls;
            lastDirectIndexCount = indexCount;
            lastDirectInstanceCount = instanceCount;
            lastDirectFirstIndex = firstIndex;
            lastDirectVertexOffset = vertexOffset;
            lastDirectFirstInstance = firstInstance;
        }
        void DrawIndirect(RHIBuffer*, uint64, uint32, uint32) override {}
        void DrawIndexedIndirect(RHIBuffer* buffer, uint64 offset, uint32 drawCount, uint32 stride) override
        {
            ++drawIndexedIndirectCalls;
            lastIndirectBuffer = buffer;
            lastIndirectOffset = offset;
            lastIndirectDrawCount = drawCount;
            lastIndirectStride = stride;
        }
        void DrawIndexedIndirectCount(RHIBuffer* buffer,
                                      uint64 offset,
                                      RHIBuffer* countBuffer,
                                      uint64 countOffset,
                                      uint32 maxDrawCount,
                                      uint32 stride) override
        {
            ++drawIndexedIndirectCountCalls;
            lastIndirectBuffer = buffer;
            lastIndirectOffset = offset;
            lastCountBuffer = countBuffer;
            lastCountOffset = countOffset;
            lastIndirectDrawCount = maxDrawCount;
            lastIndirectStride = stride;
        }
        void Dispatch(uint32 groupCountX, uint32 groupCountY, uint32 groupCountZ) override
        {
            dispatches.push_back({groupCountX, groupCountY, groupCountZ});
        }
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

        uint32 drawIndexedCalls = 0;
        uint32 drawIndexedIndirectCalls = 0;
        uint32 drawIndexedIndirectCountCalls = 0;
        uint32 lastDirectIndexCount = 0;
        uint32 lastDirectInstanceCount = 0;
        uint32 lastDirectFirstIndex = 0;
        int32 lastDirectVertexOffset = 0;
        uint32 lastDirectFirstInstance = 0;
        RHIBuffer* lastIndirectBuffer = nullptr;
        RHIBuffer* lastCountBuffer = nullptr;
        uint64 lastIndirectOffset = 0;
        uint64 lastCountOffset = 0;
        uint32 lastIndirectDrawCount = 0;
        uint32 lastIndirectStride = 0;
        std::vector<RHIBufferBarrier> bufferBarriers;
        std::vector<std::array<uint32, 3>> dispatches;
        std::vector<RHIPipeline*> pipelines;
        std::vector<RHIDescriptorSet*> descriptorSets;
    };

    class FakeDevice final : public IRHIDevice
    {
    public:
        FakeDevice()
        {
            capabilities.indexedIndirectExecution.supportsFixedCount = true;
            capabilities.indexedIndirectExecution.supportsCountBuffer = true;
            capabilities.supportsIndirectDrawCount = true;
            capabilities.indexedIndirectExecution.supportsFirstInstance = true;
            capabilities.indexedIndirectExecution.requiresExactCommandStride = true;
            capabilities.indexedIndirectExecution.indexedCommandSize =
                sizeof(IndirectDrawIndexedCommand);
            capabilities.indexedIndirectExecution.minCommandStride =
                sizeof(IndirectDrawIndexedCommand);
            capabilities.indexedIndirectExecution.commandStrideAlignment = 4;
            capabilities.indexedIndirectExecution.argumentOffsetAlignment = 4;
            capabilities.indexedIndirectExecution.countOffsetAlignment = 4;
            capabilities.indexedIndirectExecution.maxDrawCount = UINT32_MAX;
            capabilities.indexedIndirectExecution.countValueSize = sizeof(uint32);
        }

        RHIBufferRef CreateBuffer(const RHIBufferDesc& desc) override
        {
            RHIBufferRef buffer(new FakeBuffer(desc, bufferLifetimeState));
            auto* fakeBuffer = static_cast<FakeBuffer*>(buffer.Get());
            if (failTransientUploadMap && desc.debugName != nullptr &&
                std::string(desc.debugName) == "GPUCulling.TransientUpload")
            {
                fakeBuffer->SetMapSucceeds(false);
            }
            createdBuffers.push_back(fakeBuffer);
            return buffer;
        }

        RHITextureRef CreateTexture(const RHITextureDesc&) override { return {}; }
        RHITextureViewRef CreateTextureView(RHITexture*, const RHITextureViewDesc& = {}) override { return {}; }
        RHISamplerRef CreateSampler(const RHISamplerDesc&) override { return {}; }
        RHIShaderRef CreateShader(const RHIShaderDesc& desc) override
        {
            return m_enablePipelineObjects ? RHIShaderRef(new FakeShader(desc)) : RHIShaderRef{};
        }
        RHIHeapRef CreateHeap(const RHIHeapDesc&) override { return {}; }
        RHITextureRef CreatePlacedTexture(RHIHeap*, uint64, const RHITextureDesc&) override { return {}; }
        RHIBufferRef CreatePlacedBuffer(RHIHeap*, uint64, const RHIBufferDesc&) override { return {}; }
        MemoryRequirements GetTextureMemoryRequirements(const RHITextureDesc&) override { return {}; }
        MemoryRequirements GetBufferMemoryRequirements(const RHIBufferDesc& desc) override { return {desc.size, 256}; }
        RHIDescriptorSetLayoutRef CreateDescriptorSetLayout(
            const RHIDescriptorSetLayoutDesc& desc) override
        {
            ++createDescriptorSetLayoutCalls;
            return m_enablePipelineObjects
                ? RHIDescriptorSetLayoutRef(new FakeDescriptorSetLayout(desc))
                : RHIDescriptorSetLayoutRef{};
        }
        RHIPipelineLayoutRef CreatePipelineLayout(const RHIPipelineLayoutDesc& desc) override
        {
            return m_enablePipelineObjects
                ? RHIPipelineLayoutRef(new FakePipelineLayout(desc))
                : RHIPipelineLayoutRef{};
        }
        RHIPipelineRef CreateGraphicsPipeline(const RHIGraphicsPipelineDesc&) override { return {}; }
        RHIPipelineRef CreateComputePipeline(const RHIComputePipelineDesc&) override
        {
            ++createComputePipelineCalls;
            return m_enablePipelineObjects
                ? RHIPipelineRef(new FakeComputePipeline())
                : RHIPipelineRef{};
        }
        RHIDescriptorSetRef CreateDescriptorSet(const RHIDescriptorSetDesc& desc) override
        {
            return m_enablePipelineObjects
                ? RHIDescriptorSetRef(new FakeDescriptorSet(desc))
                : RHIDescriptorSetRef{};
        }
        RHIQueryPoolRef CreateQueryPool(const RHIQueryPoolDesc&) override { return {}; }
        RHICommandContextRef CreateCommandContext(RHICommandQueueType) override { return RHICommandContextRef(new FakeCommandContext()); }
        uint64 SubmitCommandContext(RHICommandContext*, RHIFence* signalFence = nullptr) override
        {
            return signalFence
                ? static_cast<FakeFence*>(signalFence)->AllocateValue()
                : 0;
        }
        uint64 SubmitCommandContexts(std::span<RHICommandContext* const>, RHIFence* = nullptr) override { return 0; }
        RHISwapChainRef CreateSwapChain(const RHISwapChainDesc&) override { return {}; }
        RHIFenceRef CreateFence(uint64 initialValue = 0) override
        {
            RHIFenceRef fence(new FakeFence(initialValue));
            fences.push_back(fence);
            return fence;
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
        const RHICapabilities& GetCapabilities() const override { return capabilities; }
        RHIBackendType GetBackendType() const override { return backendType; }

        FakeBuffer* FindBuffer(const char* name) const
        {
            for (FakeBuffer* buffer : createdBuffers)
            {
                if (buffer && buffer->GetDebugName() == name)
                    return buffer;
            }
            return nullptr;
        }

        void EnableTimelineRetirement()
        {
            capabilities.backendType = RHIBackendType::DX12;
            capabilities.adapterName = "GPUDrivenLifetimeFake";
            capabilities.driverVersion = "1";
            capabilities.supportsComputePipeline = true;
            capabilities.supportsDescriptorSets = true;
            capabilities.supportsDynamicDescriptorOffsets = true;
            capabilities.maxDescriptorSets = 4;
            capabilities.supportsExplicitResourceBarriers = true;
            capabilities.supportsDefaultQueueFenceSignal = true;
            capabilities.supportsExplicitQueueFenceSignal = true;
            capabilities.supportsAsyncCompute = true;
            capabilities.dx12.resourceBindingTier = 2;
            capabilities.queueTopology.completionMode = RHIQueueCompletionMode::NativeTimeline;
            capabilities.queueTopology.logicalQueueDomains = {
                GPUQueueDomain::Graphics,
                GPUQueueDomain::Compute,
                GPUQueueDomain::Copy};
            capabilities.queueTopology.activeDomainCount = 3;
        }

        void EnableGPUScenePipelineObjects()
        {
            capabilities.supportsComputePipeline = true;
            capabilities.supportsDescriptorSets = true;
            capabilities.indexedIndirectExecution.supportsCountBuffer = true;
            m_enablePipelineObjects = true;
        }

        FakeFence* GetFence(size_t index) const
        {
            return index < fences.size()
                ? static_cast<FakeFence*>(fences[index].Get())
                : nullptr;
        }

        RHICapabilities capabilities;
        RHIBackendType backendType = RHIBackendType::DX12;
        std::vector<FakeBuffer*> createdBuffers;
        std::vector<RHIFenceRef> fences;
        std::shared_ptr<BufferLifetimeState> bufferLifetimeState;
        uint32 createDescriptorSetLayoutCalls = 0;
        uint32 createComputePipelineCalls = 0;
        bool failTransientUploadMap = false;

    private:
        bool m_enablePipelineObjects = false;
    };

    class FakeEncodedCommandBuffer final : public RHIEncodedCommandBuffer
    {
    public:
        explicit FakeEncodedCommandBuffer(RHIBackendType backendType)
            : m_backendType(backendType)
        {
        }

        RHIBackendType GetBackendType() const override { return m_backendType; }

    private:
        RHIBackendType m_backendType = RHIBackendType::None;
    };

    class FakeEncodedSubmissionStrategy final : public IRenderSubmissionStrategy
    {
    public:
        RenderSubmissionResult Submit(
            RHICommandContext&,
            const RenderSubmissionRequest& request) const override
        {
            RenderSubmissionResult result;
            if (request.kind != RenderSubmissionKind::EncodedCommandBuffer)
            {
                result.validationCode =
                    RenderSubmissionValidationCode::UnsupportedSubmissionKind;
                return result;
            }
            if (request.encodedCommandBuffer.commandBuffer == nullptr ||
                request.encodedCommandBuffer.commandBuffer->GetBackendType() !=
                    RHIBackendType::Metal)
            {
                result.validationCode = RenderSubmissionValidationCode::InvalidRequest;
                return result;
            }
            result.recorded = true;
            result.submittedDrawUpperBound = 1;
            return result;
        }
    };

    RenderSubmissionResult RecordIndexedIndirectSubmission(
        FakeCommandContext& context,
        const GPUCullingIndexedIndirectSubmission& cullingSubmission)
    {
        RenderSubmissionRequest request;
        request.kind = RenderSubmissionKind::IndexedIndirect;
        request.indexedIndirect = cullingSubmission.execution;
        request.capabilities = cullingSubmission.capabilities;
        const IndexedIndirectRenderSubmissionStrategy strategy;
        return strategy.Submit(context, request);
    }

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

    GPUSceneResidentGraphLease MakeGPUSceneLease(
        FakeDevice& device,
        uint64 version,
        const std::array<uint32, GPU_SCENE_RESIDENT_TABLE_COUNT>& capacities)
    {
        const std::array<uint32, GPU_SCENE_RESIDENT_TABLE_COUNT> rowStrides{
            sizeof(GPUScenePrimitiveRow),
            sizeof(GPUSceneBoundsRow),
            sizeof(GPUSceneTransformRow),
            sizeof(GPUSceneMaterialRow),
            sizeof(GPUSceneGeometryRow),
            sizeof(GPUSceneDrawMetadataRow)};
        GPUSceneResidentGraphLease lease;
        lease.version = version;
        for (uint32 tableIndex = 0;
             tableIndex < GPU_SCENE_RESIDENT_TABLE_COUNT;
             ++tableIndex)
        {
            RHIBufferDesc desc;
            desc.size =
                static_cast<uint64>(capacities[tableIndex]) * rowStrides[tableIndex];
            desc.usage = RHIBufferUsage::Structured | RHIBufferUsage::ShaderResource;
            desc.memoryType = RHIMemoryType::Upload;
            desc.stride = rowStrides[tableIndex];
            desc.debugName = "GPUDrivenValidation.GPUSceneLeaseTable";
            lease.buffers[tableIndex] = device.CreateBuffer(desc);
            lease.handles[tableIndex].index = tableIndex;
            lease.capacities[tableIndex] = capacities[tableIndex];
        }
        return lease;
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

        const std::istreambuf_iterator<char> begin(file);
        const std::istreambuf_iterator<char> end;
        std::string contents(begin, end);
        contents.erase(
            std::remove(contents.begin(), contents.end(), '\r'),
            contents.end());
        return contents;
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

TEST_F(GPUDrivenValidationFixture, DX12QualificationIsCandidateUntilProductionGatesClose)
{
    const GPUDrivenBackendQualification qualification =
        GetGPUDrivenBackendQualification(RHIBackendType::DX12);

    EXPECT_EQ(RVX_GPU_DRIVEN_QUALIFICATION_SCHEMA_VERSION,
              qualification.schemaVersion);
    EXPECT_EQ(RHIBackendType::DX12, qualification.backend);
    EXPECT_EQ(2u, qualification.revision);
    EXPECT_EQ(GPUDrivenQualificationLevel::Candidate,
              qualification.GetLevel());
    EXPECT_FALSE(qualification.IsQualified());
    EXPECT_TRUE(qualification.HasPassed(
        GPUDrivenQualificationGate::RHIContractConformance));
    EXPECT_TRUE(qualification.HasPassed(
        GPUDrivenQualificationGate::MultiBatchMaterialRouting));
    EXPECT_TRUE(qualification.HasPassed(
        GPUDrivenQualificationGate::GPUBasedValidation));
    EXPECT_TRUE(qualification.HasPassed(
        GPUDrivenQualificationGate::CrossPathImageParity));
    EXPECT_FALSE(qualification.HasPassed(
        GPUDrivenQualificationGate::RealAssetRegression));
    EXPECT_FALSE(qualification.HasPassed(
        GPUDrivenQualificationGate::AdapterDriverMatrix));

    const uint64 expectedMissingGateMask =
        GetGPUDrivenQualificationGateMask(
            GPUDrivenQualificationGate::RealAssetRegression) |
        GetGPUDrivenQualificationGateMask(
            GPUDrivenQualificationGate::AdapterDriverMatrix);
    EXPECT_EQ(expectedMissingGateMask, qualification.GetMissingGateMask());
}

TEST_F(GPUDrivenValidationFixture, VulkanQualificationIsCandidateAndAutoRemainsDirect)
{
    const GPUDrivenBackendQualification qualification =
        GetGPUDrivenBackendQualification(RHIBackendType::Vulkan);
    EXPECT_EQ(2u, qualification.revision);
    EXPECT_EQ(GPUDrivenQualificationLevel::Candidate,
              qualification.GetLevel());
    EXPECT_FALSE(qualification.IsQualified());
    const uint64 expectedPassedGateMask =
        GetGPUDrivenQualificationGateMask(
            GPUDrivenQualificationGate::RHIContractConformance) |
        GetGPUDrivenQualificationGateMask(
            GPUDrivenQualificationGate::ShaderPipelineContracts) |
        GetGPUDrivenQualificationGateMask(
            GPUDrivenQualificationGate::DescriptorIntegrity) |
        GetGPUDrivenQualificationGateMask(
            GPUDrivenQualificationGate::ResourceStateValidation) |
        GetGPUDrivenQualificationGateMask(
            GPUDrivenQualificationGate::IndirectExecutionSmoke) |
        GetGPUDrivenQualificationGateMask(
            GPUDrivenQualificationGate::DirectFallbackSmoke) |
        GetGPUDrivenQualificationGateMask(
            GPUDrivenQualificationGate::RepeatedFrameResize) |
        GetGPUDrivenQualificationGateMask(
            GPUDrivenQualificationGate::CrossPathImageParity);
    EXPECT_EQ(expectedPassedGateMask, qualification.passedGateMask);
    EXPECT_TRUE(qualification.HasPassed(
        GPUDrivenQualificationGate::RHIContractConformance));
    EXPECT_TRUE(qualification.HasPassed(
        GPUDrivenQualificationGate::ShaderPipelineContracts));
    EXPECT_TRUE(qualification.HasPassed(
        GPUDrivenQualificationGate::IndirectExecutionSmoke));
    EXPECT_TRUE(qualification.HasPassed(
        GPUDrivenQualificationGate::RepeatedFrameResize));
    EXPECT_TRUE(qualification.HasPassed(
        GPUDrivenQualificationGate::CrossPathImageParity));
    EXPECT_FALSE(qualification.HasPassed(
        GPUDrivenQualificationGate::GPUBasedValidation));

    GPUDrivenPolicyInput input;
    input.requestedMode = RenderGPUDrivenMode::Auto;
    input.backend = RHIBackendType::Vulkan;
    input.supportsComputePipeline = true;
    input.supportsDescriptorSets = true;
    input.indexedIndirectExecution.supportsCountBuffer = true;
    input.pipelineReady = true;
    const GPUDrivenPolicyDecision decision = ResolveGPUDrivenPolicy(input);
    EXPECT_FALSE(decision.enabled);
    EXPECT_EQ(GPUDrivenPolicyReason::BackendNotQualified, decision.reason);
}

TEST_F(GPUDrivenValidationFixture,
       GPUCullingUsesSemanticCapabilitiesInsteadOfADx12BackendGate)
{
    FakeDevice device;
    device.backendType = RHIBackendType::Vulkan;
    device.capabilities.backendType = RHIBackendType::Vulkan;
    device.EnableGPUScenePipelineObjects();

    GPUCullingConfig config;
    config.maxInstances = 4;
    GPUCulling culling;
    culling.Initialize(&device, config);

    const GPUCullingExecutionDecision decision = culling.GetExecutionDecision();
    EXPECT_NE(decision.fallbackReason,
              GPUCullingFallbackReason::ShaderBackendUnsupported);
}

TEST_F(GPUDrivenValidationFixture, RemainingBackendsRemainUnqualified)
{
    for (RHIBackendType backend : {
             RHIBackendType::Metal,
             RHIBackendType::DX11,
             RHIBackendType::OpenGL,
             RHIBackendType::Auto,
             RHIBackendType::None})
    {
        const GPUDrivenBackendQualification qualification =
            GetGPUDrivenBackendQualification(backend);
        EXPECT_EQ(GPUDrivenQualificationLevel::Unqualified,
                  qualification.GetLevel());
        EXPECT_FALSE(qualification.IsQualified());
        EXPECT_EQ(0u, qualification.revision);
        EXPECT_EQ(0u, qualification.passedGateMask);
        EXPECT_EQ(RVX_GPU_DRIVEN_REQUIRED_QUALIFICATION_GATE_MASK,
                  qualification.GetMissingGateMask());
    }
}

TEST_F(GPUDrivenValidationFixture, QualificationIsDerivedAndMalformedRecordsFailClosed)
{
    GPUDrivenBackendQualification qualification =
        GetGPUDrivenBackendQualification(RHIBackendType::DX12);
    qualification.revision = 3;
    qualification.passedGateMask = qualification.requiredGateMask;

    EXPECT_TRUE(qualification.IsValidManifest());
    EXPECT_TRUE(qualification.IsQualified());
    EXPECT_EQ(GPUDrivenQualificationLevel::Qualified,
              qualification.GetLevel());

    qualification.passedGateMask &= ~GetGPUDrivenQualificationGateMask(
        GPUDrivenQualificationGate::RealAssetRegression);
    EXPECT_TRUE(qualification.IsValidManifest());
    EXPECT_FALSE(qualification.IsQualified());
    EXPECT_EQ(GPUDrivenQualificationLevel::Candidate,
              qualification.GetLevel());

    qualification.passedGateMask |= 1ull << 63;
    EXPECT_FALSE(qualification.IsValidManifest());
    EXPECT_FALSE(qualification.IsQualified());
    EXPECT_EQ(GPUDrivenQualificationLevel::Unqualified,
              qualification.GetLevel());

    qualification.passedGateMask = qualification.requiredGateMask;
    ++qualification.schemaVersion;
    EXPECT_FALSE(qualification.IsValidManifest());
    EXPECT_FALSE(qualification.IsQualified());
    EXPECT_EQ(GPUDrivenQualificationLevel::Unqualified,
              qualification.GetLevel());
}

TEST_F(GPUDrivenValidationFixture, AutoModeRequiresAQualifiedBackend)
{
    GPUDrivenPolicyInput input;
    input.requestedMode = RenderGPUDrivenMode::Auto;
    input.backend = RHIBackendType::DX12;
    input.supportsComputePipeline = true;
    input.supportsDescriptorSets = true;
    input.indexedIndirectExecution.supportsCountBuffer = true;
    input.pipelineReady = true;

    const GPUDrivenPolicyDecision decision = ResolveGPUDrivenPolicy(input);
    EXPECT_FALSE(decision.enabled);
    EXPECT_FALSE(decision.backendQualified);
    EXPECT_EQ(GPUDrivenQualificationLevel::Candidate,
              decision.qualificationLevel);
    EXPECT_EQ(2u, decision.qualificationRevision);
    EXPECT_NE(0u, decision.passedQualificationGateMask);
    EXPECT_NE(0u, decision.missingQualificationGateMask);
    EXPECT_TRUE(decision.capabilitiesReady);
    EXPECT_TRUE(decision.pipelineReady);
    EXPECT_EQ(GPUDrivenPolicyReason::BackendNotQualified, decision.reason);
}

TEST_F(GPUDrivenValidationFixture, ForceEnabledBypassesQualificationButNotCapabilities)
{
    GPUDrivenPolicyInput input;
    input.requestedMode = RenderGPUDrivenMode::ForceEnabled;
    input.backend = RHIBackendType::DX12;
    input.supportsComputePipeline = true;
    input.supportsDescriptorSets = true;
    input.indexedIndirectExecution.supportsCountBuffer = true;
    input.pipelineReady = true;

    GPUDrivenPolicyDecision decision = ResolveGPUDrivenPolicy(input);
    EXPECT_TRUE(decision.enabled);
    EXPECT_EQ(GPUDrivenPolicyReason::None, decision.reason);

    input.indexedIndirectExecution.supportsCountBuffer = false;
    decision = ResolveGPUDrivenPolicy(input);
    EXPECT_FALSE(decision.enabled);
    EXPECT_EQ(GPUDrivenPolicyReason::IndirectDrawCountUnsupported, decision.reason);
}

TEST_F(GPUDrivenValidationFixture, ForceDisabledAlwaysSelectsDirectRendering)
{
    GPUDrivenPolicyInput input;
    input.requestedMode = RenderGPUDrivenMode::ForceDisabled;
    input.backend = RHIBackendType::DX12;
    input.supportsComputePipeline = true;
    input.supportsDescriptorSets = true;
    input.indexedIndirectExecution.supportsCountBuffer = true;
    input.pipelineReady = true;

    const GPUDrivenPolicyDecision decision = ResolveGPUDrivenPolicy(input);
    EXPECT_FALSE(decision.enabled);
    EXPECT_EQ(GPUDrivenPolicyReason::ForcedDisabled, decision.reason);
}

TEST_F(GPUDrivenValidationFixture, InvalidModeFailsClosed)
{
    GPUDrivenPolicyInput input;
    input.requestedMode = static_cast<RenderGPUDrivenMode>(0xFFU);
    input.backend = RHIBackendType::DX12;
    input.supportsComputePipeline = true;
    input.supportsDescriptorSets = true;
    input.indexedIndirectExecution.supportsCountBuffer = true;
    input.pipelineReady = true;

    const GPUDrivenPolicyDecision decision = ResolveGPUDrivenPolicy(input);
    EXPECT_FALSE(decision.enabled);
    EXPECT_EQ(GPUDrivenPolicyReason::InvalidMode, decision.reason);
}

TEST_F(GPUDrivenValidationFixture, InjectedQualificationMatchesLegacyAndRejectsMalformedEvidence)
{
    GPUDrivenPolicyInput input;
    input.requestedMode = RenderGPUDrivenMode::Auto;
    input.backend = RHIBackendType::DX12;
    input.supportsComputePipeline = true;
    input.supportsDescriptorSets = true;
    input.indexedIndirectExecution.supportsCountBuffer = true;
    input.pipelineReady = true;

    const GPUDrivenBackendQualification qualification =
        GetGPUDrivenBackendQualification(input.backend);
    const GPUDrivenPolicyDecision legacy = ResolveGPUDrivenPolicy(input);
    const GPUDrivenPolicyDecision injected = ResolveGPUDrivenPolicy(
        input, qualification);
    EXPECT_EQ(legacy.enabled, injected.enabled);
    EXPECT_EQ(legacy.reason, injected.reason);
    EXPECT_EQ(legacy.qualificationLevel, injected.qualificationLevel);
    EXPECT_EQ(legacy.passedQualificationGateMask,
              injected.passedQualificationGateMask);

    input.requestedMode = RenderGPUDrivenMode::ForceEnabled;
    GPUDrivenBackendQualification malformed = qualification;
    malformed.schemaVersion++;
    const GPUDrivenPolicyDecision malformedDecision = ResolveGPUDrivenPolicy(
        input, malformed);
    EXPECT_FALSE(malformedDecision.enabled);
    EXPECT_EQ(GPUDrivenPolicyReason::QualificationInvalid,
              malformedDecision.reason);
}

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

    ASSERT_EQ(4u, culling.GetIndirectCommands().size());
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

    const GPUCullingIndexedIndirectSubmission cullingSubmission =
        culling.BuildIndexedIndirectGroupSubmission(0);
    EXPECT_EQ(RHIIndirectExecutionMode::FixedCount,
              cullingSubmission.execution.mode);
    EXPECT_EQ(culling.GetIndirectBuffer(),
              cullingSubmission.execution.argumentBuffer);
    EXPECT_EQ(sizeof(IndirectDrawIndexedCommand),
              cullingSubmission.execution.commandStride);
    EXPECT_TRUE(cullingSubmission.execution.requiresFirstInstance);
    const RenderSubmissionResult submission =
        RecordIndexedIndirectSubmission(ctx, cullingSubmission);
    EXPECT_TRUE(submission.recorded);
    EXPECT_EQ(1u, submission.submittedDrawUpperBound);
    EXPECT_TRUE(submission.executedDrawCountAvailable);
    EXPECT_EQ(1u, submission.executedDrawCount);
    EXPECT_EQ(1u, ctx.drawIndexedIndirectCalls);
    EXPECT_EQ(culling.GetIndirectBuffer(), ctx.lastIndirectBuffer);
    EXPECT_EQ(0u, ctx.lastIndirectOffset);
    EXPECT_EQ(1u, ctx.lastIndirectDrawCount);
    EXPECT_EQ(sizeof(IndirectDrawIndexedCommand), ctx.lastIndirectStride);
}

TEST_F(GPUDrivenValidationFixture,
       FrameSlotsIsolatePersistentInputsAndRestoreTheirAccessSnapshots)
{
    FakeDevice device;

    GPUCullingConfig config;
    config.maxInstances = 4;

    GPUCulling culling;
    culling.Initialize(&device, config, 2);
    ASSERT_TRUE(culling.IsInitialized());
    ASSERT_EQ(2u, culling.GetFrameSlotCount());

    ASSERT_TRUE(culling.SetFrameSlot(0));
    RHIBuffer* const slot0InstanceBuffer = culling.GetInstanceBuffer();
    RHIBuffer* const slot0ConstantsBuffer = culling.GetCullingConstantsBuffer();
    ASSERT_NE(nullptr, slot0InstanceBuffer);
    ASSERT_NE(nullptr, slot0ConstantsBuffer);

    culling.BeginFrame();
    ASSERT_EQ(0u, culling.AddInstance(
        MakeInstance(Vec3(1.0f, 0.0f, -5.0f), 1.0f, 36)));
    culling.EndFrame();
    const GPUInstanceData slot0InitialInstance = ReadBufferValue<GPUInstanceData>(
        *static_cast<const FakeBuffer*>(slot0InstanceBuffer));

    FakeCommandContext context;
    culling.Cull(context, TestView(), TestProjection());
    const Mat4 slot0InitialConstants = ReadBufferValue<Mat4>(
        *static_cast<const FakeBuffer*>(slot0ConstantsBuffer));

    GPUCullingAccessSnapshots slot0Snapshots = culling.GetAccessSnapshots();
    slot0Snapshots.instances = MakeRHIBufferAccessSnapshot(
        RHIResourceState::ShaderResource,
        RHIShaderStage::Vertex,
        GPUQueueDomain::Graphics,
        RHIContentValidity::Valid);
    slot0Snapshots.constants = MakeRHIBufferAccessSnapshot(
        RHIResourceState::ConstantBuffer,
        RHIShaderStage::Compute,
        GPUQueueDomain::Graphics,
        RHIContentValidity::Valid);
    culling.CommitAccessSnapshots(slot0Snapshots);

    ASSERT_TRUE(culling.SetFrameSlot(1));
    RHIBuffer* const slot1InstanceBuffer = culling.GetInstanceBuffer();
    RHIBuffer* const slot1ConstantsBuffer = culling.GetCullingConstantsBuffer();
    ASSERT_NE(nullptr, slot1InstanceBuffer);
    ASSERT_NE(nullptr, slot1ConstantsBuffer);
    EXPECT_NE(slot0InstanceBuffer, slot1InstanceBuffer);
    EXPECT_NE(slot0ConstantsBuffer, slot1ConstantsBuffer);

    culling.BeginFrame();
    ASSERT_EQ(0u, culling.AddInstance(
        MakeInstance(Vec3(9.0f, 0.0f, -5.0f), 1.0f, 24)));
    culling.EndFrame();
    const GPUInstanceData slot1InitialInstance = ReadBufferValue<GPUInstanceData>(
        *static_cast<const FakeBuffer*>(slot1InstanceBuffer));

    const Mat4 slot1View = lookAt(Vec3(2.0f, 0.0f, 0.0f),
                                  Vec3(2.0f, 0.0f, -1.0f),
                                  Vec3(0.0f, 1.0f, 0.0f));
    culling.Cull(context, slot1View, TestProjection());
    const Mat4 slot1InitialConstants = ReadBufferValue<Mat4>(
        *static_cast<const FakeBuffer*>(slot1ConstantsBuffer));

    GPUCullingAccessSnapshots slot1Snapshots = culling.GetAccessSnapshots();
    slot1Snapshots.instances = MakeRHIBufferAccessSnapshot(
        RHIResourceState::ShaderResource,
        RHIShaderStage::Compute,
        GPUQueueDomain::Graphics,
        RHIContentValidity::Valid);
    slot1Snapshots.constants = MakeRHIBufferAccessSnapshot(
        RHIResourceState::ConstantBuffer,
        RHIShaderStage::All,
        GPUQueueDomain::Graphics,
        RHIContentValidity::Valid);
    culling.CommitAccessSnapshots(slot1Snapshots);

    ASSERT_TRUE(culling.SetFrameSlot(0));
    EXPECT_EQ(slot0InstanceBuffer, culling.GetInstanceBuffer());
    EXPECT_EQ(slot0ConstantsBuffer, culling.GetCullingConstantsBuffer());
    EXPECT_EQ(slot0Snapshots.instances, culling.GetAccessSnapshots().instances);
    EXPECT_EQ(slot0Snapshots.constants, culling.GetAccessSnapshots().constants);
    const GPUInstanceData slot0InstanceAfterSlot1 = ReadBufferValue<GPUInstanceData>(
        *static_cast<const FakeBuffer*>(slot0InstanceBuffer));
    const Mat4 slot0ConstantsAfterSlot1 = ReadBufferValue<Mat4>(
        *static_cast<const FakeBuffer*>(slot0ConstantsBuffer));
    EXPECT_EQ(0, std::memcmp(&slot0InitialInstance,
                             &slot0InstanceAfterSlot1,
                             sizeof(GPUInstanceData)));
    EXPECT_EQ(0, std::memcmp(&slot0InitialConstants,
                             &slot0ConstantsAfterSlot1,
                             sizeof(Mat4)));

    culling.BeginFrame();
    ASSERT_EQ(0u, culling.AddInstance(
        MakeInstance(Vec3(-4.0f, 0.0f, -5.0f), 1.0f, 12)));
    culling.EndFrame();
    const GPUInstanceData slot0WrappedInstance = ReadBufferValue<GPUInstanceData>(
        *static_cast<const FakeBuffer*>(slot0InstanceBuffer));
    EXPECT_NE(0, std::memcmp(&slot0InitialInstance,
                             &slot0WrappedInstance,
                             sizeof(GPUInstanceData)));

    ASSERT_TRUE(culling.SetFrameSlot(1));
    EXPECT_EQ(slot1InstanceBuffer, culling.GetInstanceBuffer());
    EXPECT_EQ(slot1ConstantsBuffer, culling.GetCullingConstantsBuffer());
    EXPECT_EQ(slot1Snapshots.instances, culling.GetAccessSnapshots().instances);
    EXPECT_EQ(slot1Snapshots.constants, culling.GetAccessSnapshots().constants);
    const GPUInstanceData slot1InstanceAfterSlot0Wrap = ReadBufferValue<GPUInstanceData>(
        *static_cast<const FakeBuffer*>(slot1InstanceBuffer));
    const Mat4 slot1ConstantsAfterSlot0Wrap = ReadBufferValue<Mat4>(
        *static_cast<const FakeBuffer*>(slot1ConstantsBuffer));
    EXPECT_EQ(0, std::memcmp(&slot1InitialInstance,
                             &slot1InstanceAfterSlot0Wrap,
                             sizeof(GPUInstanceData)));
    EXPECT_EQ(0, std::memcmp(&slot1InitialConstants,
                             &slot1ConstantsAfterSlot0Wrap,
                             sizeof(Mat4)));

    const RHIBuffer* const slot1InstanceBeforeInvalidSelect = culling.GetInstanceBuffer();
    const RHIBuffer* const slot1ConstantsBeforeInvalidSelect = culling.GetCullingConstantsBuffer();
    EXPECT_FALSE(culling.SetFrameSlot(2));
    EXPECT_EQ(1u, culling.GetActiveFrameSlot());
    EXPECT_EQ(slot1InstanceBeforeInvalidSelect, culling.GetInstanceBuffer());
    EXPECT_EQ(slot1ConstantsBeforeInvalidSelect, culling.GetCullingConstantsBuffer());
}

TEST_F(GPUDrivenValidationFixture,
       SealedRecordingStateOwnsSlotInputsAcrossSourceMutation)
{
    FakeDevice device;
    GPUCullingConfig config;
    config.maxInstances = 4;

    GPUCulling culling;
    culling.Initialize(&device, config, 2);
    ASSERT_TRUE(culling.SetFrameSlot(1));
    culling.BeginFrame();
    ASSERT_EQ(0u, culling.AddInstance(
        MakeInstance(Vec3(0.0f, 0.0f, -5.0f), 1.0f, 36)));
    culling.EndFrame();

    const GPUCullingRecordingIdentity identity{
        101u, 7u, 88u, 1u, 5u};
    const std::shared_ptr<GPUCullingRecordedState> recorded =
        culling.SealForGraph(identity);
    ASSERT_NE(nullptr, recorded);
    ASSERT_TRUE(recorded->IsValid());
    EXPECT_TRUE(recorded->Matches(identity));
    EXPECT_EQ(1u, recorded->GetSourceFrameSlot());
    ASSERT_NE(nullptr, recorded->GetCulling().GetInstanceBuffer());
    EXPECT_NE(culling.GetInstanceBuffer(),
              recorded->GetCulling().GetInstanceBuffer());

    // Reuse the source slot before graph execution. The sealed state must
    // retain the old slot contents, groups, and output resources.
    culling.BeginFrame();
    ASSERT_EQ(0u, culling.AddInstance(
        MakeInstance(Vec3(0.0f, 0.0f, -5.0f), 1.0f, 12)));
    culling.EndFrame();

    const GPUInstanceData sealedInstance = ReadBufferValue<GPUInstanceData>(
        *static_cast<const FakeBuffer*>(recorded->GetCulling().GetInstanceBuffer()));
    const GPUInstanceData mutatedSourceInstance = ReadBufferValue<GPUInstanceData>(
        *static_cast<const FakeBuffer*>(culling.GetInstanceBuffer()));
    EXPECT_EQ(36u, sealedInstance.indexCount);
    EXPECT_EQ(12u, mutatedSourceInstance.indexCount);

    FakeCommandContext context;
    recorded->Cull(context, TestView(), TestProjection());
    const GPUCulling& sealedCulling = recorded->GetCulling();
    EXPECT_TRUE(sealedCulling.WasCpuFallbackUsedLastCull());
    ASSERT_EQ(1u, sealedCulling.GetIndirectCommands().size());
    EXPECT_EQ(36u, sealedCulling.GetIndirectCommands()[0].indexCount);
}

TEST_F(GPUDrivenValidationFixture,
       SealedRecordingSubmissionRetainsGpuObjectsUntilCompletion)
{
    FakeDevice device;
    device.EnableTimelineRetirement();
    device.bufferLifetimeState = std::make_shared<BufferLifetimeState>();

    RenderSubmissionTracker tracker;
    ASSERT_TRUE(tracker.Initialize(&device));
    RenderRetirementQueue retirement;
    ASSERT_TRUE(retirement.Initialize(&tracker));

    GPUCullingConfig config;
    config.maxInstances = 4;
    GPUCulling culling;
    culling.Initialize(&device, config);
    culling.BeginFrame();
    ASSERT_EQ(0u, culling.AddInstance(
        MakeInstance(Vec3(0.0f, 0.0f, -5.0f), 1.0f, 36)));
    culling.EndFrame();

    const GPUCullingRecordingIdentity identity{
        201u, 11u, 99u, 0u, 6u};
    std::shared_ptr<GPUCullingRecordedState> recorded =
        culling.SealForGraph(identity);
    ASSERT_NE(nullptr, recorded);

    RenderSubmissionResourceBatch batch;
    ASSERT_TRUE(recorded->RetainSubmissionResources(batch));

    // This CPU-fallback fixture has exactly the active instance/constants
    // pair plus the five shared culling/indirect buffers. A future omission is
    // therefore observable as a smaller retained set.
    constexpr uint32 expectedSealedPrimaryObjectCount = 7;
    EXPECT_EQ(expectedSealedPrimaryObjectCount,
              batch.GetRetainedObjectCount());

    const uint32 destroyedBeforeStateRelease =
        device.bufferLifetimeState->destroyedCount;
    recorded.reset();
    EXPECT_EQ(destroyedBeforeStateRelease,
              device.bufferLifetimeState->destroyedCount);

    FakeCommandContext context;
    GPUCompletionToken completion;
    const GPUCompletionPoint submittedPoint = tracker.Submit(&context);
    ASSERT_TRUE(InsertGPUCompletionPoint(completion, submittedPoint));
    batch.SealAndTransfer(completion, retirement);
    EXPECT_EQ(retirement.GetDiagnostics().entryCount,
              expectedSealedPrimaryObjectCount);
    EXPECT_EQ(retirement.Poll(), GPUCompletionStatus::Pending);
    EXPECT_EQ(destroyedBeforeStateRelease,
              device.bufferLifetimeState->destroyedCount);

    FakeFence* fence = device.GetFence(0);
    ASSERT_NE(nullptr, fence);
    fence->Complete(submittedPoint.value);
    EXPECT_EQ(retirement.Poll(), GPUCompletionStatus::Completed);
    EXPECT_EQ(destroyedBeforeStateRelease + expectedSealedPrimaryObjectCount,
              device.bufferLifetimeState->destroyedCount);
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
    device.capabilities.indexedIndirectExecution.supportsCountBuffer = true;

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
        device.capabilities.indexedIndirectExecution.supportsCountBuffer = true;

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
        device.capabilities.indexedIndirectExecution.supportsCountBuffer = true;

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

TEST_F(GPUDrivenValidationFixture,
       GpuCullingFallbackReasonPreservesExistingDiagnosticValues)
{
    EXPECT_EQ(4u, static_cast<uint32>(
        GPUCullingFallbackReason::IndirectDrawCountUnsupported));
    EXPECT_EQ(12u, static_cast<uint32>(
        GPUCullingFallbackReason::PipelineResourcesUnavailable));
    EXPECT_EQ(13u, static_cast<uint32>(
        GPUCullingFallbackReason::IndirectDrawFirstInstanceUnsupported));
}

TEST_F(GPUDrivenValidationFixture,
       GpuExecutionRejectsMissingFirstInstanceBeforePipelineCreationOrRecording)
{
    FakeDevice device;
    device.capabilities.supportsComputePipeline = true;
    device.capabilities.supportsDescriptorSets = true;
    device.capabilities.indexedIndirectExecution.supportsCountBuffer = true;
    device.capabilities.indexedIndirectExecution.supportsFirstInstance = false;

    GPUCulling culling;
    culling.Initialize(&device, GPUCullingConfig{});

    const GPUCullingExecutionDecision decision = culling.GetExecutionDecision();
    EXPECT_EQ(GPUCullingExecutionMode::CpuFallback, decision.mode);
    EXPECT_FALSE(decision.gpuCapable);
    EXPECT_FALSE(decision.pipelineReady);
    EXPECT_EQ(GPUCullingFallbackReason::IndirectDrawFirstInstanceUnsupported,
              decision.fallbackReason);
    EXPECT_EQ(0u, device.createDescriptorSetLayoutCalls);
    EXPECT_EQ(0u, device.createComputePipelineCalls);

    culling.BeginFrame();
    EXPECT_EQ(0u, culling.AddInstance(
        MakeInstance(Vec3(0.0f, 0.0f, -5.0f), 1.0f, 36)));
    culling.EndFrame();

    FakeCommandContext context;
    culling.Cull(context, TestView(), TestProjection());
    EXPECT_TRUE(culling.WasCpuFallbackUsedLastCull());
    EXPECT_FALSE(culling.WasGpuExecutionUsedLastCull());
    EXPECT_EQ(GPUCullingFallbackReason::IndirectDrawFirstInstanceUnsupported,
              culling.GetLastFallbackReason());
    EXPECT_TRUE(context.dispatches.empty());
}

TEST_F(GPUDrivenValidationFixture,
       IndexedIndirectCountSubmissionRejectsUnsupportedCapabilityWithoutRecording)
{
    FakeDevice device;
    device.capabilities.indexedIndirectExecution.supportsCountBuffer = false;

    RHIBufferDesc argumentDesc;
    argumentDesc.size = sizeof(IndirectDrawIndexedCommand);
    argumentDesc.usage = RHIBufferUsage::IndirectArgs;
    argumentDesc.memoryType = RHIMemoryType::Default;
    argumentDesc.stride = sizeof(IndirectDrawIndexedCommand);
    RHIBufferRef argumentBuffer = device.CreateBuffer(argumentDesc);
    ASSERT_NE(nullptr, argumentBuffer.Get());

    RHIBufferDesc countDesc;
    countDesc.size = sizeof(uint32);
    countDesc.usage = RHIBufferUsage::IndirectArgs;
    countDesc.memoryType = RHIMemoryType::Default;
    countDesc.stride = sizeof(uint32);
    RHIBufferRef countBuffer = device.CreateBuffer(countDesc);
    ASSERT_NE(nullptr, countBuffer.Get());

    GPUCullingIndexedIndirectSubmission cullingSubmission;
    cullingSubmission.capabilities = &device.capabilities;
    cullingSubmission.execution.mode = RHIIndirectExecutionMode::CountBuffer;
    cullingSubmission.execution.argumentBuffer = argumentBuffer.Get();
    cullingSubmission.execution.commandStride = sizeof(IndirectDrawIndexedCommand);
    cullingSubmission.execution.maxDrawCount = 1;
    cullingSubmission.execution.requiresFirstInstance = true;
    cullingSubmission.execution.countBuffer = countBuffer.Get();

    FakeCommandContext context;
    const RenderSubmissionResult result =
        RecordIndexedIndirectSubmission(context, cullingSubmission);
    EXPECT_FALSE(result.recorded);
    EXPECT_EQ(RenderSubmissionValidationCode::IndexedIndirectValidationFailed,
              result.validationCode);
    EXPECT_EQ(RHIIndexedIndirectExecutionValidationCode::CapabilityUnsupported,
              result.indexedIndirectValidationCode);
    EXPECT_EQ(0u, context.drawIndexedIndirectCalls);
    EXPECT_EQ(0u, context.drawIndexedIndirectCountCalls);
}

TEST_F(GPUDrivenValidationFixture, DrawIndexedIndirectHonorsMaxDrawCount)
{
    FakeDevice device;
    GPUCulling culling;
    culling.Initialize(&device, GPUCullingConfig{});

    culling.BeginFrame();
    EXPECT_EQ(0u, culling.AddInstance(MakeInstance(Vec3(0.0f, 0.0f, -5.0f), 1.0f, 12)));
    EXPECT_EQ(1u, culling.AddInstance(MakeInstance(Vec3(1.0f, 0.0f, -5.0f), 1.0f, 12)));
    culling.EndFrame();

    FakeCommandContext ctx;
    culling.Cull(ctx, TestView(), TestProjection());
    ASSERT_EQ(1u, culling.GetDrawCount());

    const GPUCullingIndexedIndirectSubmission cullingSubmission =
        culling.BuildIndexedIndirectSubmission(1);
    EXPECT_EQ(RHIIndirectExecutionMode::FixedCount,
              cullingSubmission.execution.mode);
    EXPECT_EQ(1u, cullingSubmission.execution.maxDrawCount);
    const RenderSubmissionResult submission =
        RecordIndexedIndirectSubmission(ctx, cullingSubmission);
    EXPECT_TRUE(submission.recorded);
    EXPECT_EQ(1u, submission.submittedDrawUpperBound);
    EXPECT_TRUE(submission.executedDrawCountAvailable);
    EXPECT_EQ(1u, submission.executedDrawCount);
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
    EXPECT_EQ(1u, culling.AddInstance(MakeInstance(Vec3(100.0f, 0.0f, -5.0f), 1.0f, 36)));
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
    EXPECT_EQ(1u, culling.GetDrawGroups()[1].commandOffset);
    EXPECT_EQ(2u, culling.GetDrawGroups()[1].visibleInstanceOffset);
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
        ReadBufferValue<IndirectDrawIndexedCommand>(*indirectBuffer, 1);
    EXPECT_EQ(36u, firstGroupCommand.indexCount);
    EXPECT_EQ(0u, firstGroupCommand.firstInstance);
    EXPECT_EQ(24u, secondGroupCommand.indexCount);
    EXPECT_EQ(2u, secondGroupCommand.firstInstance);

    const FakeBuffer* visibleBuffer =
        device.FindBuffer("GPUCulling.VisibleInstanceBuffer");
    ASSERT_NE(nullptr, visibleBuffer);
    EXPECT_EQ(0u, ReadBufferValue<uint32>(*visibleBuffer, 0));
    EXPECT_EQ(RVX_INVALID_INDEX, ReadBufferValue<uint32>(*visibleBuffer, 1));
    EXPECT_EQ(2u, ReadBufferValue<uint32>(*visibleBuffer, 2));

    const GPUCullingIndexedIndirectSubmission wholeSubmission =
        culling.BuildIndexedIndirectSubmission();
    EXPECT_EQ(nullptr, wholeSubmission.execution.argumentBuffer);
    EXPECT_EQ(0u, ctx.drawIndexedIndirectCalls);

    const GPUCullingIndexedIndirectSubmission firstCullingSubmission =
        culling.BuildIndexedIndirectGroupSubmission(0);
    EXPECT_EQ(RHIIndirectExecutionMode::FixedCount,
              firstCullingSubmission.execution.mode);
    EXPECT_TRUE(firstCullingSubmission.execution.requiresFirstInstance);
    EXPECT_EQ(0u, firstCullingSubmission.execution.argumentOffset);
    const RenderSubmissionResult firstSubmission =
        RecordIndexedIndirectSubmission(ctx, firstCullingSubmission);
    EXPECT_TRUE(firstSubmission.recorded);
    EXPECT_EQ(1u, firstSubmission.submittedDrawUpperBound);
    EXPECT_TRUE(firstSubmission.executedDrawCountAvailable);
    EXPECT_EQ(1u, firstSubmission.executedDrawCount);
    EXPECT_EQ(0u, ctx.lastIndirectOffset);
    EXPECT_EQ(1u, ctx.lastIndirectDrawCount);
    const GPUCullingIndexedIndirectSubmission secondCullingSubmission =
        culling.BuildIndexedIndirectGroupSubmission(1);
    EXPECT_TRUE(secondCullingSubmission.execution.requiresFirstInstance);
    EXPECT_EQ(sizeof(IndirectDrawIndexedCommand),
              secondCullingSubmission.execution.argumentOffset);
    const RenderSubmissionResult secondSubmission =
        RecordIndexedIndirectSubmission(ctx, secondCullingSubmission);
    EXPECT_TRUE(secondSubmission.recorded);
    EXPECT_EQ(1u, secondSubmission.submittedDrawUpperBound);
    EXPECT_TRUE(secondSubmission.executedDrawCountAvailable);
    EXPECT_EQ(1u, secondSubmission.executedDrawCount);
    EXPECT_EQ(sizeof(IndirectDrawIndexedCommand), ctx.lastIndirectOffset);
    EXPECT_EQ(1u, ctx.lastIndirectDrawCount);
}

TEST(RenderSubmissionStrategyValidation, DirectStrategyRecordsExactResultSemantics)
{
    FakeCommandContext context;
    RenderSubmissionRequest request;
    request.kind = RenderSubmissionKind::DirectIndexed;
    request.directIndexed = {36u, 2u, 7u, -3, 4u};

    const DirectRenderSubmissionStrategy strategy;
    const RenderSubmissionResult result = strategy.Submit(context, request);

    EXPECT_EQ(RenderSubmissionValidationCode::Success, result.validationCode);
    EXPECT_TRUE(result.recorded);
    EXPECT_EQ(1u, result.submittedDrawUpperBound);
    EXPECT_TRUE(result.executedDrawCountAvailable);
    EXPECT_EQ(1u, result.executedDrawCount);
    EXPECT_EQ(1u, context.drawIndexedCalls);
    EXPECT_EQ(36u, context.lastDirectIndexCount);
    EXPECT_EQ(2u, context.lastDirectInstanceCount);
    EXPECT_EQ(7u, context.lastDirectFirstIndex);
    EXPECT_EQ(-3, context.lastDirectVertexOffset);
    EXPECT_EQ(4u, context.lastDirectFirstInstance);
}

TEST(RenderSubmissionStrategyValidation, IndexedStrategyPreservesFixedAndCountSemantics)
{
    FakeDevice device;
    RHIBufferDesc argumentDesc;
    argumentDesc.size = sizeof(IndirectDrawIndexedCommand) * 2u;
    argumentDesc.usage = RHIBufferUsage::IndirectArgs;
    FakeBuffer argumentBuffer(argumentDesc);

    RHIBufferDesc countDesc;
    countDesc.size = sizeof(uint32);
    countDesc.usage = RHIBufferUsage::IndirectArgs;
    FakeBuffer countBuffer(countDesc);

    FakeCommandContext context;
    RenderSubmissionRequest fixedRequest;
    fixedRequest.kind = RenderSubmissionKind::IndexedIndirect;
    fixedRequest.capabilities = &device.capabilities;
    fixedRequest.indexedIndirect.argumentBuffer = &argumentBuffer;
    fixedRequest.indexedIndirect.commandStride = sizeof(IndirectDrawIndexedCommand);
    fixedRequest.indexedIndirect.maxDrawCount = 2;

    const IndexedIndirectRenderSubmissionStrategy strategy;
    const RenderSubmissionResult fixedResult = strategy.Submit(context, fixedRequest);
    EXPECT_EQ(RenderSubmissionValidationCode::Success, fixedResult.validationCode);
    EXPECT_TRUE(fixedResult.recorded);
    EXPECT_EQ(2u, fixedResult.submittedDrawUpperBound);
    EXPECT_TRUE(fixedResult.executedDrawCountAvailable);
    EXPECT_EQ(2u, fixedResult.executedDrawCount);
    EXPECT_EQ(1u, context.drawIndexedIndirectCalls);
    EXPECT_EQ(0u, context.drawIndexedIndirectCountCalls);

    RenderSubmissionRequest countRequest = fixedRequest;
    countRequest.indexedIndirect.mode = RHIIndirectExecutionMode::CountBuffer;
    countRequest.indexedIndirect.countBuffer = &countBuffer;
    const RenderSubmissionResult countResult = strategy.Submit(context, countRequest);
    EXPECT_EQ(RenderSubmissionValidationCode::Success, countResult.validationCode);
    EXPECT_TRUE(countResult.recorded);
    EXPECT_EQ(2u, countResult.submittedDrawUpperBound);
    EXPECT_FALSE(countResult.executedDrawCountAvailable);
    EXPECT_EQ(0u, countResult.executedDrawCount);
    EXPECT_EQ(1u, context.drawIndexedIndirectCalls);
    EXPECT_EQ(1u, context.drawIndexedIndirectCountCalls);
}

TEST(RenderSubmissionStrategyValidation, IndexedStrategyZeroAndInvalidRequestsNeverRecord)
{
    FakeDevice device;
    FakeCommandContext context;
    const IndexedIndirectRenderSubmissionStrategy strategy;

    RenderSubmissionRequest zeroRequest;
    zeroRequest.kind = RenderSubmissionKind::IndexedIndirect;
    zeroRequest.capabilities = &device.capabilities;
    zeroRequest.indexedIndirect.maxDrawCount = 0;
    const RenderSubmissionResult zeroResult = strategy.Submit(context, zeroRequest);
    EXPECT_EQ(RenderSubmissionValidationCode::Success, zeroResult.validationCode);
    EXPECT_FALSE(zeroResult.recorded);
    EXPECT_TRUE(zeroResult.executedDrawCountAvailable);
    EXPECT_EQ(0u, zeroResult.executedDrawCount);

    RenderSubmissionRequest invalidRequest = zeroRequest;
    invalidRequest.indexedIndirect.maxDrawCount = 1;
    const RenderSubmissionResult invalidResult = strategy.Submit(context, invalidRequest);
    EXPECT_EQ(RenderSubmissionValidationCode::IndexedIndirectValidationFailed,
              invalidResult.validationCode);
    EXPECT_EQ(RHIIndexedIndirectExecutionValidationCode::MissingArgumentBuffer,
              invalidResult.indexedIndirectValidationCode);
    EXPECT_FALSE(invalidResult.recorded);
    EXPECT_EQ(0u, context.drawIndexedIndirectCalls);
    EXPECT_EQ(0u, context.drawIndexedIndirectCountCalls);
}

TEST(RenderSubmissionStrategyValidation, EncodedExtensionUsesFrozenTypedRequestBoundary)
{
    FakeCommandContext context;
    FakeEncodedCommandBuffer encodedCommandBuffer(RHIBackendType::Metal);
    RenderSubmissionRequest request;
    request.kind = RenderSubmissionKind::EncodedCommandBuffer;
    request.encodedCommandBuffer.commandBuffer = &encodedCommandBuffer;

    const DirectRenderSubmissionStrategy directStrategy;
    EXPECT_EQ(RenderSubmissionValidationCode::UnsupportedSubmissionKind,
              directStrategy.Submit(context, request).validationCode);
    const IndexedIndirectRenderSubmissionStrategy indexedStrategy;
    EXPECT_EQ(RenderSubmissionValidationCode::UnsupportedSubmissionKind,
              indexedStrategy.Submit(context, request).validationCode);

    const FakeEncodedSubmissionStrategy encodedStrategy;
    const RenderSubmissionResult result = encodedStrategy.Submit(context, request);
    EXPECT_EQ(RenderSubmissionValidationCode::Success, result.validationCode);
    EXPECT_TRUE(result.recorded);
    EXPECT_EQ(1u, result.submittedDrawUpperBound);
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

    ASSERT_EQ(3u, culling.GetIndirectCommands().size());
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

TEST_F(GPUDrivenValidationFixture, SceneRendererWiresMeshDrawPacketsBeforeTypedPassRecording)
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
    const std::string subsystemSource =
        ReadTextFile(root / "Render" / "Private" / "RenderSubsystem.cpp");
    const std::string renderCMake =
        ReadTextFile(root / "Render" / "CMakeLists.txt");
    const std::string skyboxHeader =
        ReadTextFile(root / "Render" / "Include" / "Render" / "Passes" / "SkyboxPass.h");
    const std::string skyboxSource =
        ReadTextFile(root / "Render" / "Private" / "Passes" / "SkyboxPass.cpp");
    const std::string transparentHeader =
        ReadTextFile(root / "Render" / "Include" / "Render" / "Passes" / "TransparentPass.h");
    const std::string transparentSource =
        ReadTextFile(root / "Render" / "Private" / "Passes" / "TransparentPass.cpp");
    ASSERT_FALSE(header.empty());
    ASSERT_FALSE(source.empty());
    ASSERT_FALSE(depthHeader.empty());
    ASSERT_FALSE(depthSource.empty());
    ASSERT_FALSE(subsystemSource.empty());
    ASSERT_FALSE(renderCMake.empty());
    ASSERT_FALSE(skyboxHeader.empty());
    ASSERT_FALSE(skyboxSource.empty());
    ASSERT_FALSE(transparentHeader.empty());
    ASSERT_FALSE(transparentSource.empty());

    EXPECT_NE(header.find("SceneGPUDrivenCullingStats"), std::string::npos);
    EXPECT_NE(header.find("graphPassAdded"), std::string::npos);
    EXPECT_NE(header.find("graphPassRecorded"), std::string::npos);
    EXPECT_NE(header.find("gpuExecutionRecorded"), std::string::npos);
    EXPECT_NE(header.find("graphInputDrawItemCount"), std::string::npos);
    EXPECT_NE(header.find("opaqueIndirectRequested"), std::string::npos);
    EXPECT_NE(header.find("opaqueGpuDrivenIndirectDrawCount"), std::string::npos);
    EXPECT_NE(header.find("opaqueGpuDrivenIndirectSubmittedDrawUpperBound"),
              std::string::npos);
    EXPECT_NE(header.find("opaqueGpuDrivenExecutedDrawCountAvailable"),
              std::string::npos);
    EXPECT_NE(header.find("SetGPUDrivenCullingMode"), std::string::npos);
    EXPECT_NE(header.find("const SceneGPUDrivenCullingStats& GetGPUDrivenCullingStats() const"),
              std::string::npos);
    EXPECT_NE(header.find("void AddGPUDrivenCullingPass("), std::string::npos);
    EXPECT_NE(header.find("void PrepareGPUDrivenGraphCullInputs()"), std::string::npos);
    EXPECT_NE(header.find("void BuildGPUDrivenVisibilityInputs()"), std::string::npos);
    EXPECT_NE(header.find("void PrepareMeshPassPackets()"), std::string::npos);
    EXPECT_NE(header.find("void CompileRenderFramePlan()"), std::string::npos);
    EXPECT_NE(header.find("RenderPolicyDiagnostics m_renderPolicyDiagnostics"),
              std::string::npos);
    EXPECT_NE(header.find("const SceneMeshPassPreparation& GetMeshPassPreparation() const"),
              std::string::npos);

    const size_t buildDrawLists = source.find("void SceneRenderer::BuildMaterialDrawLists()");
    ASSERT_NE(buildDrawLists, std::string::npos);
    const size_t prepareMeshDefinition =
        source.find("void SceneRenderer::PrepareMeshPassPackets()", buildDrawLists);
    ASSERT_NE(prepareMeshDefinition, std::string::npos);
    const std::string buildMaterialSegment =
        source.substr(buildDrawLists, prepareMeshDefinition - buildDrawLists);
    const size_t prepareMeshPasses = buildMaterialSegment.find("PrepareMeshPassPackets();");
    ASSERT_NE(prepareMeshPasses, std::string::npos);
    EXPECT_LT(prepareMeshPasses, prepareMeshDefinition - buildDrawLists);
    EXPECT_EQ(buildMaterialSegment.find("BuildGPUDrivenVisibilityInputs();"), std::string::npos);
    EXPECT_EQ(buildMaterialSegment.find("m_objectVelocityPass->SetRenderScene"), std::string::npos);
    EXPECT_EQ(buildMaterialSegment.find("m_objectVelocityPass->SetRenderTargets"), std::string::npos);
    EXPECT_EQ(buildMaterialSegment.find("m_objectVelocityPass->SetDrawItems"), std::string::npos);

    EXPECT_FALSE(std::filesystem::exists(
        root / "Render" / "Private" / "Renderer" / "RenderFrameResourceBinder.h"));
    EXPECT_FALSE(std::filesystem::exists(
        root / "Render" / "Private" / "Renderer" / "RenderFrameResourceBinder.cpp"));
    EXPECT_EQ(header.find("UpdatePassResources"), std::string::npos);
    EXPECT_EQ(header.find("ExecutePasses"), std::string::npos);
    EXPECT_EQ(source.find("RenderFrameResourceBinder"), std::string::npos);
    EXPECT_EQ(source.find("UpdatePassResources"), std::string::npos);
    EXPECT_EQ(source.find("ExecutePasses"), std::string::npos);
    EXPECT_EQ(renderCMake.find("RenderFrameResourceBinder"), std::string::npos);
    EXPECT_EQ(source.find("m_skyboxPass->SetCubemap"), std::string::npos);
    EXPECT_EQ(source.find("m_skyboxPass->SetProceduralSkyParams"), std::string::npos);
    EXPECT_EQ(source.find("m_skyboxPass->SetSolidColor"), std::string::npos);
    EXPECT_EQ(source.find("m_skyboxPass->ClearSkybox"), std::string::npos);
    EXPECT_EQ(skyboxHeader.find("SetCubemap"), std::string::npos);
    EXPECT_EQ(skyboxHeader.find("SetProceduralSkyParams"), std::string::npos);
    EXPECT_EQ(skyboxHeader.find("SetSolidColor"), std::string::npos);
    EXPECT_EQ(skyboxHeader.find("ClearSkybox"), std::string::npos);
    EXPECT_EQ(skyboxHeader.find("SetRenderTargets"), std::string::npos);
    EXPECT_EQ(skyboxSource.find("SkyboxPass::SetRenderTargets"), std::string::npos);
    EXPECT_EQ(skyboxSource.find("SkyboxPass::SetCubemap"), std::string::npos);
    EXPECT_EQ(skyboxSource.find("SkyboxPass::SetProceduralSkyParams"), std::string::npos);
    EXPECT_EQ(skyboxSource.find("SkyboxPass::SetSolidColor"), std::string::npos);
    EXPECT_EQ(skyboxSource.find("SkyboxPass::ClearSkybox"), std::string::npos);
    EXPECT_EQ(transparentHeader.find("SetRenderScene"), std::string::npos);
    EXPECT_EQ(transparentHeader.find("SetRenderTargets"), std::string::npos);
    EXPECT_EQ(transparentSource.find("TransparentPass::SetRenderScene"), std::string::npos);
    EXPECT_EQ(transparentSource.find("TransparentPass::SetRenderTargets"), std::string::npos);

    const size_t renderDefinition = source.find("void SceneRenderer::Render()");
    const size_t compileCall = source.find("CompileRenderFramePlan();", renderDefinition);
    const size_t cullingCall = source.find(
        "BuildGPUDrivenVisibilityInputs();", compileCall);
    const size_t clearGraph = source.find("m_renderGraph->Clear();", cullingCall);
    const size_t buildGraphCall = source.find("BuildRenderGraph();", clearGraph);
    ASSERT_NE(renderDefinition, std::string::npos);
    ASSERT_NE(compileCall, std::string::npos);
    ASSERT_NE(cullingCall, std::string::npos);
    ASSERT_NE(clearGraph, std::string::npos);
    ASSERT_NE(buildGraphCall, std::string::npos);
    EXPECT_LT(compileCall, cullingCall);
    EXPECT_LT(cullingCall, clearGraph);
    EXPECT_LT(clearGraph, buildGraphCall);

    const size_t setModeDefinition =
        source.find("void SceneRenderer::SetGPUDrivenCullingMode");
    const size_t setEnabledDefinition =
        source.find("void SceneRenderer::SetGPUDrivenCullingEnabled", setModeDefinition);
    ASSERT_NE(setModeDefinition, std::string::npos);
    ASSERT_NE(setEnabledDefinition, std::string::npos);
    const std::string setModeBody = source.substr(
        setModeDefinition, setEnabledDefinition - setModeDefinition);
    EXPECT_EQ(setModeBody.find("ResolveGPUDrivenPolicy"), std::string::npos);
    EXPECT_EQ(setModeBody.find("GetExecutionDecision"), std::string::npos);

    const size_t applyDefinition =
        source.find("void SceneRenderer::PrepareFrameApplyResources");
    const size_t invalidateCall =
        source.find("InvalidateRenderFramePlan();", applyDefinition);
    const size_t registryAssignment =
        source.find("m_renderResourceRegistry = &registry;", applyDefinition);
    ASSERT_NE(applyDefinition, std::string::npos);
    ASSERT_NE(invalidateCall, std::string::npos);
    ASSERT_NE(registryAssignment, std::string::npos);
    EXPECT_LT(invalidateCall, registryAssignment);
    EXPECT_NE(subsystemSource.find("features.policy = frame.policy;"),
              std::string::npos);
    EXPECT_EQ(depthSource.find("GetBackendType"), std::string::npos);
    EXPECT_EQ(depthSource.find("GetGPUDrivenBackendQualification"),
              std::string::npos);

    EXPECT_EQ(source.find("ApplyGPUDrivenCullingToDrawLists"), std::string::npos);
    EXPECT_EQ(source.find("ApplyGPUDrivenCullingToDrawList"), std::string::npos);
    EXPECT_EQ(source.find("m_gpuCulling->CullCpuFallback"), std::string::npos);
    EXPECT_EQ(source.find("getSourceDrawItem"), std::string::npos);
    EXPECT_NE(source.find("reference.sourcePacketIndex"), std::string::npos);
    EXPECT_NE(source.find("m_renderCandidates.Find(pass, sourcePacketIndex)"),
              std::string::npos);
    EXPECT_NE(header.find("m_depthGPUCulling"), std::string::npos);
    EXPECT_NE(header.find("m_opaqueGPUCulling"), std::string::npos);
    EXPECT_NE(source.find("gpuCullingFrameSlotCount"), std::string::npos);
    EXPECT_NE(source.find("frameSynchronizer->GetFrameCount()"),
              std::string::npos);
    const size_t depthSlotSelection = source.find(
        "m_depthGPUCulling->SetFrameSlot(frameSlot)");
    const size_t opaqueSlotSelection = source.find(
        "m_opaqueGPUCulling->SetFrameSlot(frameSlot)");
    ASSERT_NE(std::string::npos, depthSlotSelection);
    ASSERT_NE(std::string::npos, opaqueSlotSelection);
    const size_t firstGpuBeginFrame = source.find("owner->BeginFrame();");
    ASSERT_NE(std::string::npos, firstGpuBeginFrame);
    EXPECT_LT(depthSlotSelection, firstGpuBeginFrame);
    EXPECT_LT(opaqueSlotSelection, firstGpuBeginFrame);
    EXPECT_EQ(std::string::npos,
              source.find("owner->BeginFrame();", firstGpuBeginFrame + 1));
    const size_t planValidation = source.find(
        "ValidatePlannedGPUDrivenPacketRange(*framePlan");
    ASSERT_NE(std::string::npos, planValidation);
    EXPECT_LT(planValidation, firstGpuBeginFrame);
    EXPECT_NE(source.find("PrepareGPUDrivenGraphCullInputs();"), std::string::npos);
    EXPECT_NE(source.find("m_meshPassPreparation.depth"), std::string::npos);
    EXPECT_NE(source.find("m_meshPassPreparation.opaque"), std::string::npos);
    EXPECT_NE(source.find("for (const RenderDrawGroupRange& group : stream.groups)"),
              std::string::npos);
    EXPECT_EQ(source.find("std::find_if(drawGroups.begin(), drawGroups.end()"),
              std::string::npos);
    EXPECT_NE(source.find("owner->BeginDrawGroup("), std::string::npos);
    EXPECT_NE(source.find("key.geometry.mesh"), std::string::npos);
    EXPECT_NE(source.find("leaderPacket.materialKey.material"), std::string::npos);
    EXPECT_NE(source.find("key.pipeline.materialVariant"), std::string::npos);

    const std::string opaqueHeader =
        ReadTextFile(root / "Render" / "Include" / "Render" / "Passes" / "OpaquePass.h");
    const std::string opaqueSource =
        ReadTextFile(root / "Render" / "Private" / "Passes" / "OpaquePass.cpp");
    EXPECT_EQ(opaqueHeader.find("FindGPUDrivenGroupRepresentative"), std::string::npos);
    EXPECT_EQ(opaqueSource.find("FindGPUDrivenGroupRepresentative"), std::string::npos);
    EXPECT_EQ(opaqueSource.find("GetBackendType"), std::string::npos);
    EXPECT_EQ(opaqueSource.find("GetGPUDrivenBackendQualification"),
              std::string::npos);

    const size_t buildGraph = source.find("void SceneRenderer::BuildRenderGraph()");
    ASSERT_NE(buildGraph, std::string::npos);
    const size_t cullGraphCall = source.find(
        "AddGPUDrivenCullingPass(passRecordContext.identity);", buildGraph);
    const size_t passRegistryLoop = source.find("for (auto& pass : m_passRegistry->GetPasses())", buildGraph);
    ASSERT_NE(cullGraphCall, std::string::npos);
    ASSERT_NE(passRegistryLoop, std::string::npos);
    EXPECT_LT(cullGraphCall, passRegistryLoop);
    const size_t typedPassRecord = source.find(
        "pass->AddToGraph(*m_renderGraph, passRecordContext);", passRegistryLoop);
    ASSERT_NE(typedPassRecord, std::string::npos);
    EXPECT_EQ(source.find("pass->AddToGraph(*m_renderGraph, m_viewData)"),
              std::string::npos);

    EXPECT_NE(source.find("\"GPUDrivenDepthCull\""), std::string::npos);
    EXPECT_NE(source.find("\"GPUDrivenOpaqueCull\""), std::string::npos);
    EXPECT_NE(source.find("RenderGraphPassType::Compute"), std::string::npos);
    EXPECT_NE(source.find("MakeRHIAccessSnapshot(RHIResourceState::ConstantBuffer"), std::string::npos);
    EXPECT_NE(source.find("MakeRHIAccessSnapshot(RHIResourceState::ShaderResource"), std::string::npos);
    EXPECT_NE(source.find("RHIResourceState::UnorderedAccess,"), std::string::npos);
    EXPECT_NE(source.find("data.indirectDraws = builder.Write(data.indirectDraws, unorderedAccess)"),
              std::string::npos);
    EXPECT_EQ(source.find("m_depthPrepass->SetGPUDriven" "RenderGraphResources"),
              std::string::npos);
    EXPECT_EQ(source.find("m_opaquePass->SetGPUDriven" "RenderGraphResources"),
              std::string::npos);
    EXPECT_NE(source.find("RenderPassRecordContext passRecordContext"), std::string::npos);
    EXPECT_NE(source.find("passRecordContext.depthGPUDriven"), std::string::npos);
    EXPECT_NE(source.find("passRecordContext.opaqueGPUDriven"), std::string::npos);
    EXPECT_NE(source.find("SealForGraph"), std::string::npos);
    EXPECT_NE(source.find("recordedState->Cull"), std::string::npos);
    EXPECT_EQ(source.find("owner->Cull(ctx, m_viewData.viewMatrix, m_viewData.projectionMatrix)"),
              std::string::npos);
    EXPECT_EQ(source.find("owner->Cull(ctx, cullViewMatrix, cullProjectionMatrix)"),
              std::string::npos);
    EXPECT_NE(source.find("cullingStatsSink->gpuExecutionRecorded"), std::string::npos);
    EXPECT_NE(source.find("m_opaquePass->GetDrawStats()"), std::string::npos);
    EXPECT_NE(source.find("m_gpuDrivenCullingStats.opaqueIndirectRequested"), std::string::npos);
    EXPECT_NE(source.find("m_gpuDrivenCullingStats.opaqueGpuDrivenIndirectDrawCount"), std::string::npos);
    EXPECT_NE(source.find(
                  "opaqueGpuDrivenIndirectSubmittedDrawUpperBound"),
              std::string::npos);
    EXPECT_EQ(depthHeader.find("SetGPUDriven" "RenderGraphResources"),
              std::string::npos);
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
    const std::string diagnosticsHeader =
        ReadTextFile(root / "Render" / "Include" / "Render" / "RenderDiagnostics.h");
    const std::string subsystemSource =
        ReadTextFile(root / "Render" / "Private" / "RenderSubsystem.cpp");
    const std::string artifactSource =
        ReadTextFile(root / "Render" / "Private" / "Diagnostics" / "RenderToolArtifacts.cpp");
    ASSERT_FALSE(header.empty());
    ASSERT_FALSE(source.empty());
    ASSERT_FALSE(diagnosticsHeader.empty());
    ASSERT_FALSE(subsystemSource.empty());
    ASSERT_FALSE(artifactSource.empty());

    EXPECT_NE(header.find("bool executionDecisionAvailable = false;"), std::string::npos);
    EXPECT_NE(header.find("GPUCullingExecutionDecision executionDecision;"), std::string::npos);
    EXPECT_NE(header.find("bool opaqueCullingReady = false;"), std::string::npos);
    EXPECT_NE(header.find("bool opaquePipelineReady = false;"), std::string::npos);
    EXPECT_NE(header.find("bool opaqueIndirectSubmitted = false;"), std::string::npos);
    EXPECT_NE(header.find("GPUDrivenDrawFallbackReason opaqueFallbackReason"),
              std::string::npos);
    EXPECT_NE(header.find("SceneGPUDrivenCullingStats gpuDrivenCullingStats;"), std::string::npos);
    EXPECT_NE(source.find("diagnostics.gpuDrivenCullingStats = m_gpuDrivenCullingStats;"),
              std::string::npos);
    EXPECT_NE(source.find("m_gpuDrivenCullingStats.executionDecisionAvailable = true;"),
              std::string::npos);
    EXPECT_NE(source.find("diagnosticGPUCulling->GetExecutionDecision()"),
              std::string::npos);
    EXPECT_NE(source.find("const GPUCullingExecutionDecision depthGPUExecution"),
              std::string::npos);
    EXPECT_NE(source.find("const GPUCullingExecutionDecision opaqueGPUExecution"),
              std::string::npos);
    EXPECT_NE(source.find("m_meshPassPreparation.depth,\n                                  depthGPUExecution"),
              std::string::npos);
    EXPECT_NE(source.find("m_meshPassPreparation.opaque,\n                                  opaqueGPUExecution"),
              std::string::npos);
    EXPECT_EQ(source.find("primaryGPUCulling"), std::string::npos);
    EXPECT_NE(source.find("m_gpuDrivenCullingStats.outputOpaqueDrawItemCount"),
              std::string::npos);
    EXPECT_NE(source.find("m_opaqueGPUCulling->GetStatistics()"),
              std::string::npos);
    for (const char* field : {"visibilityCandidateCount",
                              "cpuVisibleCandidateCount",
                              "passVisibilityCandidateCount",
                              "gpuPlannedVisibilityCandidateCount",
                              "invalidVisibilityBoundsCount",
                              "gpuDeferredVisibilityCandidateCount",
                              "gpuVisibilityReadbackPerformed",
                              "occlusionRequestedButUnavailable",
                              "gpuCullingGraphPassCount"})
    {
        EXPECT_NE(header.find(field), std::string::npos);
        EXPECT_NE(diagnosticsHeader.find(field), std::string::npos);
        EXPECT_NE(subsystemSource.find(std::string("RVX_COPY_GPU_CULLING_FIELD(") + field + ")"),
                  std::string::npos);
    }
    EXPECT_NE(artifactSource.find("gpuPlannedCandidates="), std::string::npos);
    EXPECT_NE(artifactSource.find("gpuDeferredCandidates="), std::string::npos);
}

TEST_F(GPUDrivenValidationFixture,
       IndependentCullingOwnersKeepDepthAndOpaqueStreamsIsolated)
{
    FakeDevice device;
    GPUCullingConfig config;
    config.maxInstances = 8;
    config.enableDistanceCulling = false;

    GPUCulling depthOwner;
    GPUCulling opaqueOwner;
    depthOwner.Initialize(&device, config);
    opaqueOwner.Initialize(&device, config);

    depthOwner.BeginFrame();
    ASSERT_EQ(0u, depthOwner.BeginDrawGroup(1001u));
    EXPECT_EQ(0u, depthOwner.AddInstance(
        MakeInstance(Vec3(0.0f, 0.0f, -4.0f), 0.5f, 3)));
    depthOwner.EndDrawGroup();
    depthOwner.EndFrame();

    opaqueOwner.BeginFrame();
    ASSERT_EQ(0u, opaqueOwner.BeginDrawGroup(2001u));
    EXPECT_EQ(0u, opaqueOwner.AddInstance(
        MakeInstance(Vec3(0.0f, 0.0f, -6.0f), 0.5f, 6)));
    EXPECT_EQ(1u, opaqueOwner.AddInstance(
        MakeInstance(Vec3(100.0f, 0.0f, -6.0f), 0.5f, 6)));
    opaqueOwner.EndDrawGroup();
    opaqueOwner.EndFrame();

    depthOwner.CullCpuFallback(TestView(), TestProjection());
    opaqueOwner.CullCpuFallback(TestView(), TestProjection());
    EXPECT_EQ(1u, depthOwner.GetInstanceCount());
    EXPECT_EQ(2u, opaqueOwner.GetInstanceCount());
    EXPECT_EQ(1u, depthOwner.GetDrawGroups().size());
    EXPECT_EQ(1u, opaqueOwner.GetDrawGroups().size());
    EXPECT_EQ(1u, depthOwner.GetDrawCount());
    EXPECT_EQ(1u, opaqueOwner.GetDrawCount());
    EXPECT_NE(depthOwner.GetInstanceBuffer(), opaqueOwner.GetInstanceBuffer());
    EXPECT_NE(depthOwner.GetIndirectBuffer(), opaqueOwner.GetIndirectBuffer());

    // A malformed/rejected next Depth frame resets only its own owner. Opaque
    // remains a valid independently prepared stream for the current frame.
    depthOwner.BeginFrame();
    EXPECT_EQ(0u, depthOwner.GetInstanceCount());
    EXPECT_EQ(2u, opaqueOwner.GetInstanceCount());
    EXPECT_EQ(1u, opaqueOwner.GetDrawGroups().size());
}

TEST_F(GPUDrivenValidationFixture,
       VisibilityCandidateInstanceRequiresStablePacketObjectIdentity)
{
    FakeDevice device;
    GPUCullingConfig config;
    config.maxInstances = 8;
    GPUCulling culling;
    culling.Initialize(&device, config);

    RenderObject object = MakeRenderObject(
        Vec3(0.0f, 0.0f, -5.0f), 1.0f, 7001u);
    object.entityId = 42u;
    RenderScene scene;
    scene.AddObject(object);

    RenderVisibilityCandidate candidate;
    candidate.candidateIndex = 5;
    candidate.sourcePacketIndex = 3;
    candidate.objectIndex = 0;
    candidate.pass = RenderPassKind::Depth;
    candidate.objectVisible = true;
    candidate.drawable = true;
    candidate.worldBounds = object.bounds;

    RenderDrawPacket packet;
    packet.objectId = object.entityId;
    packet.primitiveData = candidate.objectIndex;
    packet.pass = candidate.pass;
    packet.geometryKey.mesh = object.mesh;
    packet.arguments.indexCount = 36;
    packet.arguments.firstIndex = 4;
    packet.arguments.vertexOffset = -2;
    const GPUIndexedDrawDesc drawDesc{36, 4, -2};

    culling.BeginFrame();
    ASSERT_EQ(0u, culling.BeginDrawGroup(7001u));
    EXPECT_EQ(0u, culling.AddVisibilityCandidateInstance(
                      scene, candidate, packet, drawDesc));

    RenderVisibilityCandidate invalidCandidate = candidate;
    invalidCandidate.candidateIndex = RVX_INVALID_INDEX;
    EXPECT_EQ(RVX_INVALID_INDEX,
              culling.AddVisibilityCandidateInstance(
                  scene, invalidCandidate, packet, drawDesc));

    RenderDrawPacket wrongPass = packet;
    wrongPass.pass = RenderPassKind::Opaque;
    EXPECT_EQ(RVX_INVALID_INDEX,
              culling.AddVisibilityCandidateInstance(
                  scene, candidate, wrongPass, drawDesc));

    RenderDrawPacket wrongObject = packet;
    wrongObject.objectId++;
    EXPECT_EQ(RVX_INVALID_INDEX,
              culling.AddVisibilityCandidateInstance(
                  scene, candidate, wrongObject, drawDesc));

    GPUIndexedDrawDesc wrongDraw = drawDesc;
    wrongDraw.firstIndex++;
    EXPECT_EQ(RVX_INVALID_INDEX,
              culling.AddVisibilityCandidateInstance(
                  scene, candidate, packet, wrongDraw));
    EXPECT_EQ(1u, culling.GetInstanceCount());
}

TEST_F(GPUDrivenValidationFixture, OcclusionRequestRemainsExplicitlyUnavailable)
{
    GPUCullingConfig defaults;
    EXPECT_FALSE(defaults.enableOcclusionCulling);
    EXPECT_FALSE(defaults.twoPhaseOcclusion);
    RenderGPUCullingSettings frameDefaults;
    EXPECT_FALSE(frameDefaults.enableOcclusionCulling);

    FakeDevice device;
    GPUCullingConfig config;
    config.enableOcclusionCulling = true;
    config.twoPhaseOcclusion = true;
    GPUCulling culling;
    culling.Initialize(&device, config);

    EXPECT_TRUE(culling.WasOcclusionRequested());
    EXPECT_FALSE(culling.IsOcclusionAvailable());
    EXPECT_FALSE(culling.GetConfig().enableOcclusionCulling);
    EXPECT_FALSE(culling.GetConfig().twoPhaseOcclusion);
}

TEST_F(GPUDrivenValidationFixture, InstanceIndexVertexStreamIsIdentityAndDrawCountsReserveNPlusOne)
{
    FakeDevice device;

    GPUCullingConfig config;
    config.maxInstances = 64;

    GPUCulling culling;
    culling.Initialize(&device, config);

    const FakeBuffer* instanceIndexBuffer =
        device.FindBuffer("GPUCulling.InstanceIndexBuffer");
    ASSERT_NE(nullptr, instanceIndexBuffer);
    EXPECT_EQ(sizeof(uint32) * 64u, instanceIndexBuffer->GetSize());
    EXPECT_EQ(sizeof(uint32), instanceIndexBuffer->GetStride());
    EXPECT_TRUE(HasFlag(instanceIndexBuffer->GetUsage(), RHIBufferUsage::Vertex));
    EXPECT_EQ(RHIMemoryType::Upload, instanceIndexBuffer->GetMemoryType());
    EXPECT_EQ(0u, ReadBufferValue<uint32>(*instanceIndexBuffer, 0));
    EXPECT_EQ(31u, ReadBufferValue<uint32>(*instanceIndexBuffer, 31));
    EXPECT_EQ(63u, ReadBufferValue<uint32>(*instanceIndexBuffer, 63));
    EXPECT_EQ(instanceIndexBuffer, culling.GetInstanceIndexBuffer());

    const FakeBuffer* visibleInstanceBuffer =
        device.FindBuffer("GPUCulling.VisibleInstanceBuffer");
    ASSERT_NE(nullptr, visibleInstanceBuffer);
    EXPECT_TRUE(HasFlag(visibleInstanceBuffer->GetUsage(),
                        RHIBufferUsage::Vertex));

    const FakeBuffer* drawCountBuffer =
        device.FindBuffer("GPUCulling.DrawCountBuffer");
    ASSERT_NE(nullptr, drawCountBuffer);
    EXPECT_GE(drawCountBuffer->GetSize(), sizeof(uint32) * 65u);

    const GPUCullingAccessSnapshots& snapshots = culling.GetAccessSnapshots();
    EXPECT_EQ(RHIResourceState::VertexBuffer,
              ProjectRHIResourceState(snapshots.instanceIndices.uniformAccess));
}

TEST_F(GPUDrivenValidationFixture, ScopedUAVBarrierCarriesMemoryDependency)
{
    const RHIAccessSnapshot unorderedAccess = MakeRHIAccessSnapshot(
        RHIResourceState::UnorderedAccess,
        RHIShaderStage::Compute,
        GPUQueueDomain::Graphics);
    const RHIBufferBarrier barrier = MakeRHIBufferBarrier(
        nullptr, unorderedAccess, unorderedAccess);

    EXPECT_TRUE(barrier.hasScopedAccess);
    EXPECT_EQ(RHIResourceState::UnorderedAccess, barrier.stateBefore);
    EXPECT_EQ(RHIResourceState::UnorderedAccess, barrier.stateAfter);
    EXPECT_TRUE(HasDependencyKind(barrier.dependencyKind,
                                  RHIDependencyKind::Memory));
}

TEST_F(GPUDrivenValidationFixture, GPUDrivenDrawFallbackReasonNamesAreStable)
{
    EXPECT_STREQ(GetGPUDrivenDrawFallbackReasonName(
                     GPUDrivenDrawFallbackReason::None),
                 "None");
    EXPECT_STREQ(GetGPUDrivenDrawFallbackReasonName(
                     GPUDrivenDrawFallbackReason::PipelineUnavailable),
                 "PipelineUnavailable");
    EXPECT_STREQ(GetGPUDrivenDrawFallbackReasonName(
                     GPUDrivenDrawFallbackReason::Disabled),
                 "Disabled");
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

    EXPECT_NE(header.find("RHIDescriptorSetRef descriptorSet"), std::string::npos);
    EXPECT_NE(header.find("std::vector<GPUCullingFrameInputs> m_frameInputs"),
              std::string::npos);
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
    EXPECT_NE(source.find("inputs.descriptorSet = m_device->CreateDescriptorSet"),
              std::string::npos);
    EXPECT_NE(source.find("ctx.SetDescriptorSet(0, inputs->descriptorSet.Get())"),
              std::string::npos);
    EXPECT_NE(source.find("BuildIndexedIndirectGroupSubmission"), std::string::npos);
    EXPECT_NE(header.find("RHIIndexedIndirectExecutionDesc"), std::string::npos);
    EXPECT_EQ(source.find("ctx.DrawIndexedIndirectCount"), std::string::npos);
    EXPECT_NE(source.find("GPUCulling.InstanceIndexBuffer"), std::string::npos);
    EXPECT_NE(source.find("RHIBufferUsage::Vertex"), std::string::npos);
    EXPECT_NE(source.find("(static_cast<uint64>(m_config.maxInstances) + 1u)"),
              std::string::npos);
    EXPECT_NE(source.find("const uint32 clearThreadCount = std::max("),
              std::string::npos);
    EXPECT_NE(source.find("static_cast<uint32>(m_drawGroups.size()) + 1u"),
              std::string::npos);
    EXPECT_NE(source.find("const auto insertCullUAVBarriers"), std::string::npos);
    EXPECT_NE(source.find("ctx.BufferBarrier(m_visibilityBuffer.Get(), computeUAVAccess, computeUAVAccess)"),
              std::string::npos);
    EXPECT_NE(source.find("ctx.BufferBarrier(m_indirectBuffer.Get(), computeUAVAccess, computeUAVAccess)"),
              std::string::npos);
    EXPECT_NE(source.find("ctx.BufferBarrier(m_drawCountBuffer.Get(), computeUAVAccess, computeUAVAccess)"),
              std::string::npos);
    const size_t frustumDispatch = source.find("ctx.Dispatch(groupCount, 1, 1);");
    const size_t cullBarriers = source.find("insertCullUAVBarriers();", frustumDispatch);
    const size_t compactDispatch = source.find("// Compact visible instances into draw commands", cullBarriers);
    ASSERT_NE(frustumDispatch, std::string::npos);
    ASSERT_NE(cullBarriers, std::string::npos);
    ASSERT_NE(compactDispatch, std::string::npos);
    EXPECT_LT(frustumDispatch, cullBarriers);
    EXPECT_LT(cullBarriers, compactDispatch);

    EXPECT_NE(rhiCommandContext.find("DrawIndexedIndirectCount"), std::string::npos);
    EXPECT_NE(shader.find("void CSFrustumCull"), std::string::npos);
    EXPECT_NE(shader.find("void CSCompactDraws"), std::string::npos);
    EXPECT_NE(shader.find("gDrawCount[instanceIndex] = 0"), std::string::npos);
    EXPECT_NE(shader.find("uint drawGroupCount = Counts.y"),
              std::string::npos);
    EXPECT_NE(shader.find("void CSFinalizeDrawGroups"), std::string::npos);
    EXPECT_NE(shader.find("gDrawCount[instance.drawGroupIndex + 1]"),
              std::string::npos);
    EXPECT_NE(shader.find("instance.drawGroupVisibleOffset + groupVisibleIndex"),
              std::string::npos);
    EXPECT_NE(shader.find("command.instanceCount = visibleCount"),
              std::string::npos);
    EXPECT_NE(shader.find("command.firstInstance = instance.drawGroupVisibleOffset"),
              std::string::npos);
}

TEST_F(GPUDrivenValidationFixture, OpaquePassDeclaresGPUDrivenDefaultLitIndirectContracts)
{
    const std::filesystem::path root = FindWorkspaceRoot();
    ASSERT_FALSE(root.empty());

    const std::string defaultLit =
        ReadTextFile(root / "Render" / "Shaders" / "DefaultLit.hlsl");
    const std::string depthOnly =
        ReadTextFile(root / "Render" / "Shaders" / "DepthOnly.hlsl");
    const std::string cullingShader =
        ReadTextFile(root / "Render" / "Shaders" / "GPUDriven" / "GPUCulling.hlsl");
    const std::string sharedInstance =
        ReadTextFile(root / "Render" / "Shaders" / "Include" / "GPUInstanceData.hlsli");
    const std::string cullingHeader =
        ReadTextFile(root / "Render" / "Include" / "Render" / "GPUDriven" / "GPUCulling.h");
    const std::string rasterInstanceHeader = ReadTextFile(
        root / "Render" / "Include" / "Render" / "Submission" /
        "RasterInstanceStream.h");
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
    ASSERT_FALSE(depthOnly.empty());
    ASSERT_FALSE(cullingShader.empty());
    ASSERT_FALSE(sharedInstance.empty());
    ASSERT_FALSE(cullingHeader.empty());
    ASSERT_FALSE(rasterInstanceHeader.empty());
    ASSERT_FALSE(pipelineHeader.empty());
    ASSERT_FALSE(pipelineSource.empty());
    ASSERT_FALSE(opaqueHeader.empty());
    ASSERT_FALSE(opaqueSource.empty());
    ASSERT_FALSE(modelViewer.empty());
    ASSERT_FALSE(testsCMake.empty());

    EXPECT_EQ(opaqueSource.find("EnsureIndirectDrawCapacity"),
              std::string::npos);
    EXPECT_EQ(opaqueSource.find("OpaquePass.IndirectDrawBuffer"),
              std::string::npos);
    EXPECT_EQ(opaqueHeader.find("SetIndirectBatchingEnabled"),
              std::string::npos);

    EXPECT_NE(defaultLit.find("StructuredBuffer<GPUInstanceData> GPUDrivenInstances"), std::string::npos);
    EXPECT_NE(defaultLit.find("PSInput VSMainGPUDriven"), std::string::npos);
    EXPECT_NE(defaultLit.find("instance.normalMatrix"), std::string::npos);
    EXPECT_NE(defaultLit.find("uint InstanceIndex : INSTANCE_INDEX"), std::string::npos);
    EXPECT_NE(defaultLit.find("GPUDrivenInstances[input.InstanceIndex]"), std::string::npos);
    EXPECT_EQ(defaultLit.find("GPUDrivenInstances[instanceId]"), std::string::npos);
    EXPECT_NE(depthOnly.find("uint InstanceIndex : INSTANCE_INDEX"), std::string::npos);
    EXPECT_NE(depthOnly.find("GPUDrivenInstances[input.InstanceIndex]"), std::string::npos);
    EXPECT_EQ(depthOnly.find("GPUDrivenInstances[instanceId]"), std::string::npos);
    EXPECT_NE(defaultLit.find("#include \"Include/GPUInstanceData.hlsli\""),
              std::string::npos);
    EXPECT_NE(depthOnly.find("#include \"Include/GPUInstanceData.hlsli\""),
              std::string::npos);
    EXPECT_NE(cullingShader.find("#include \"../Include/GPUInstanceData.hlsli\""),
              std::string::npos);
    EXPECT_EQ(defaultLit.find("struct GPUInstanceData"), std::string::npos);
    EXPECT_EQ(depthOnly.find("struct GPUInstanceData"), std::string::npos);
    EXPECT_EQ(cullingShader.find("struct GPUInstanceData"), std::string::npos);
    EXPECT_NE(sharedInstance.find("uint candidateIndex;"), std::string::npos);
    EXPECT_NE(sharedInstance.find("uint forceVisible;"), std::string::npos);
    EXPECT_NE(sharedInstance.find("uint2 padding;"), std::string::npos);
    EXPECT_NE(rasterInstanceHeader.find("struct alignas(16) GPUInstanceData"),
              std::string::npos);
    EXPECT_NE(rasterInstanceHeader.find("sizeof(GPUInstanceData) == 224"),
              std::string::npos);
    EXPECT_NE(rasterInstanceHeader.find("alignof(GPUInstanceData) == 16"),
              std::string::npos);
    EXPECT_NE(rasterInstanceHeader.find("offsetof(GPUInstanceData, forceVisible) == 212"),
              std::string::npos);
    EXPECT_NE(rasterInstanceHeader.find("offsetof(GPUInstanceData, padding) == 216"),
              std::string::npos);
    EXPECT_NE(pipelineHeader.find("GetGPUDrivenPipelineForVariant"), std::string::npos);
    EXPECT_NE(pipelineSource.find("GPUDrivenOpaquePipeline"), std::string::npos);
    EXPECT_NE(pipelineSource.find("RVX_PIPELINE_PURPOSE_GPU_DRIVEN_DEFAULT"), std::string::npos);
    EXPECT_NE(pipelineSource.find("AddElement(\"INSTANCE_INDEX\", RHIFormat::R32_UINT, 6)"),
              std::string::npos);
    EXPECT_NE(pipelineSource.find("instanceIndexElement.perInstance = true"), std::string::npos);
    EXPECT_NE(pipelineSource.find("instanceIndexElement.instanceDataStepRate = 1"), std::string::npos);
    EXPECT_EQ(opaqueHeader.find("SetGPUDriven" "CullingSource"),
              std::string::npos);
    EXPECT_EQ(opaqueHeader.find("SetGPUDriven" "RenderGraphResources"),
              std::string::npos);
    EXPECT_NE(opaqueSource.find("TryDrawGPUDrivenIndirect"), std::string::npos);
    EXPECT_NE(opaqueSource.find("BuildIndexedIndirectGroupSubmission"), std::string::npos);
    EXPECT_NE(opaqueSource.find("IndexedIndirectRenderSubmissionStrategy"), std::string::npos);
    EXPECT_NE(opaqueSource.find("ctx.SetVertexBuffer(6, m_gpuCulling->GetVisibleInstanceBuffer())"),
              std::string::npos);
    EXPECT_NE(opaqueSource.find("TransitionGPUDrivenGroupMaterialTextures"),
              std::string::npos);
    EXPECT_NE(opaqueSource.find("for (const GPUCullingDrawGroup& group : gpuCulling.GetDrawGroups())"),
              std::string::npos);
    EXPECT_EQ(opaqueSource.find("TransitionVisibleMaterialTextures"),
              std::string::npos);
    EXPECT_NE(modelViewer.find("--expect-gpu-driven-culling-ready"), std::string::npos);
    EXPECT_NE(modelViewer.find("--expect-gpu-driven-direct-ready"), std::string::npos);
    EXPECT_NE(modelViewer.find("--gpu-driven-culling-test-scene"), std::string::npos);
    EXPECT_NE(modelViewer.find("--gpu-driven <auto|on|off>"), std::string::npos);
    EXPECT_NE(modelViewer.find("--disable-gpu-driven-culling"), std::string::npos);
    EXPECT_NE(modelViewer.find(
                  "frameSettings.gpuCulling.mode = options.gpuDrivenMode"),
              std::string::npos);
    EXPECT_NE(modelViewer.find("IsGPUDrivenCullingReady"), std::string::npos);
    EXPECT_NE(modelViewer.find("IsGPUDrivenDirectFallbackReady"), std::string::npos);
    EXPECT_NE(modelViewer.find("opaquePipelineReady"), std::string::npos);
    EXPECT_NE(modelViewer.find("opaqueIndirectSubmitted"), std::string::npos);
    EXPECT_NE(modelViewer.find("opaqueDirectDrawCount"), std::string::npos);
    EXPECT_NE(modelViewer.find("opaqueFallbackReason"), std::string::npos);
    EXPECT_NE(modelViewer.find("opaqueGpuDrivenIndirectDrawCount"), std::string::npos);
    EXPECT_NE(modelViewer.find("cpuReferenceCulledDrawItemCount"),
              std::string::npos);
    EXPECT_NE(testsCMake.find("ModelViewerGPUDrivenSmoke"), std::string::npos);
    EXPECT_NE(testsCMake.find("ModelViewerGPUDrivenDisabledSmoke"), std::string::npos);
    EXPECT_NE(testsCMake.find("GPUDrivenVisualGoldenValidation"), std::string::npos);
    EXPECT_NE(testsCMake.find("GPUDrivenDisabledVisualDiffValidation"), std::string::npos);
    EXPECT_NE(testsCMake.find("GPUDrivenCrossPathVisualParityValidation"), std::string::npos);
    EXPECT_NE(testsCMake.find("--max-different-pixels 0"), std::string::npos);
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

    options.entryPoint = "CSFinalizeDrawGroups";
    ShaderCompileResult finalizeResult = compiler->Compile(options);
    ASSERT_TRUE(finalizeResult.success) << finalizeResult.errorMessage;
    EXPECT_FALSE(finalizeResult.bytecode.empty());
}

TEST_F(GPUDrivenValidationFixture,
       GPUCullingComputeShaderEntriesCompileForVulkanWithExpectedLayout)
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
    options.targetBackend = RHIBackendType::Vulkan;
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
    ASSERT_FALSE(frustumResult.bytecode.empty());
    ASSERT_TRUE(frustumResult.reflection.valid);
    ASSERT_EQ(frustumResult.bytecode.size() % sizeof(uint32), 0u);

    std::vector<uint32> spirvWords(
        frustumResult.bytecode.size() / sizeof(uint32));
    std::memcpy(spirvWords.data(), frustumResult.bytecode.data(),
                frustumResult.bytecode.size());
    ASSERT_GE(spirvWords.size(), 5u);
    ASSERT_EQ(spirvWords[0], 0x07230203u);

    bool hasInstanceArrayStride224 = false;
    bool hasStaleInstanceArrayStride216 = false;
    for (size_t wordIndex = 5; wordIndex < spirvWords.size();)
    {
        const uint16 wordCount = static_cast<uint16>(spirvWords[wordIndex] >> 16u);
        const uint16 opcode = static_cast<uint16>(spirvWords[wordIndex] & 0xFFFFu);
        ASSERT_NE(wordCount, 0u);
        ASSERT_LE(wordIndex + wordCount, spirvWords.size());

        // OpDecorate <target-id> ArrayStride <literal>.
        if (opcode == 71u && wordCount >= 4u && spirvWords[wordIndex + 2] == 6u)
        {
            hasInstanceArrayStride224 |= spirvWords[wordIndex + 3] == 224u;
            hasStaleInstanceArrayStride216 |= spirvWords[wordIndex + 3] == 216u;
        }
        wordIndex += wordCount;
    }
    EXPECT_TRUE(hasInstanceArrayStride224);
    EXPECT_FALSE(hasStaleInstanceArrayStride216);
    const auto hasBinding = [&frustumResult](uint32 binding,
                                             RHIBindingType type)
    {
        return std::any_of(
            frustumResult.reflection.resources.begin(),
            frustumResult.reflection.resources.end(),
            [binding, type](const ShaderReflection::ResourceBinding& resource)
            {
                return resource.set == 0 && resource.binding == binding &&
                       resource.type == type;
            });
    };
    EXPECT_TRUE(hasBinding(0, RHIBindingType::UniformBuffer));
    EXPECT_TRUE(hasBinding(1, RHIBindingType::ShaderResourceBuffer));
    EXPECT_TRUE(hasBinding(2, RHIBindingType::StorageBuffer));
    EXPECT_TRUE(hasBinding(5, RHIBindingType::StorageBuffer));

    options.entryPoint = "CSCompactDraws";
    ShaderCompileResult compactResult = compiler->Compile(options);
    ASSERT_TRUE(compactResult.success) << compactResult.errorMessage;
    EXPECT_FALSE(compactResult.bytecode.empty());
    EXPECT_TRUE(compactResult.reflection.valid);

    options.entryPoint = "CSFinalizeDrawGroups";
    ShaderCompileResult finalizeResult = compiler->Compile(options);
    ASSERT_TRUE(finalizeResult.success) << finalizeResult.errorMessage;
    EXPECT_FALSE(finalizeResult.bytecode.empty());
}

TEST_F(GPUDrivenValidationFixture,
       GPUSceneCandidateAbiAndSealedInputPlumbingRemainRendererPrivate)
{
    EXPECT_EQ(48u, sizeof(GPUSceneCullingCandidate));

    FakeDevice device;
    device.EnableTimelineRetirement();
    device.EnableGPUScenePipelineObjects();
    RenderSubmissionTracker tracker;
    ASSERT_TRUE(tracker.Initialize(&device));
    RenderRetirementQueue retirement;
    ASSERT_TRUE(retirement.Initialize(&tracker));
    GPUCullingConfig config;
    config.maxInstances = 4;
    GPUCulling culling;
    culling.Initialize(&device, config);
    EXPECT_EQ(nullptr, culling.GetGPUSceneCandidateBuffer());
    ASSERT_TRUE(culling.IsGPUSceneExecutionReady());

    culling.BeginFrame();
    ASSERT_EQ(0u, culling.AddInstance(
        MakeInstance(Vec3(0.0f, 0.0f, -5.0f), 1.0f, 36)));

    GPUSceneCullingCandidate candidate;
    candidate.primitiveSlot = 1u;
    candidate.primitiveGeneration = 7u;
    candidate.drawSlot = 3u;
    candidate.drawGeneration = 7u;
    candidate.objectIdLow = 42u;
    candidate.requiredPassMask = 2u;
    candidate.drawGroupIndex = 0u;
    candidate.drawGroupVisibleOffset = 0u;
    candidate.rasterInstanceIndex = 0u;
    candidate.materialParameterSlot = 3u;
    GPUSceneCullingCandidate multiPassCandidate = candidate;
    multiPassCandidate.requiredPassMask = 3u;
    EXPECT_FALSE(culling.AddGPUSceneCandidate(multiPassCandidate, 19u));
    EXPECT_TRUE(culling.AddGPUSceneCandidate(candidate, 19u));

    ASSERT_EQ(1u, culling.AddInstance(
        MakeInstance(Vec3(1.0f, 0.0f, -5.0f), 1.0f, 36)));
    GPUSceneCullingCandidate secondCandidate = candidate;
    secondCandidate.drawSlot = 4u;
    secondCandidate.rasterInstanceIndex = 1u;
    // This candidate has passed all positional/group checks and can fail only
    // at the committed-version lock.
    EXPECT_FALSE(culling.AddGPUSceneCandidate(secondCandidate, 20u));
    EXPECT_FALSE(culling.HasCompleteGPUSceneCandidates());
    EXPECT_TRUE(culling.AddGPUSceneCandidate(secondCandidate, 19u));
    EXPECT_TRUE(culling.HasCompleteGPUSceneCandidates());
    const uint32 tier1InstanceCount = culling.GetInstanceCount();
    const size_t tier1GroupCount = culling.GetDrawGroups().size();
    EXPECT_FALSE(culling.HasCompleteGPUSceneCandidates(18u));
    EXPECT_FALSE(culling.HasCompleteGPUSceneCandidates(20u));
    EXPECT_TRUE(culling.HasCompleteGPUSceneCandidates(19u));
    EXPECT_EQ(tier1InstanceCount, culling.GetInstanceCount());
    EXPECT_EQ(tier1GroupCount, culling.GetDrawGroups().size());

    culling.EndFrame();
    const GPUCullingRecordingIdentity identity{301u, 12u, 100u, 0u, 7u};
    const std::array<uint32, GPU_SCENE_RESIDENT_TABLE_COUNT> capacities{
        8u, 9u, 10u, 11u, 12u, 13u};
    GPUSceneResidentGraphLease lease = MakeGPUSceneLease(device, 19u, capacities);
    ASSERT_TRUE(lease.IsValid());

    const std::shared_ptr<GPUCullingRecordedState> recorded =
        culling.SealForGPUSceneGraph(identity, lease);
    ASSERT_NE(nullptr, recorded);
    EXPECT_TRUE(recorded->GetCulling().HasCompleteGPUSceneCandidates());
    EXPECT_TRUE(recorded->GetCulling().HasCompleteGPUSceneCandidates(19u));
    EXPECT_FALSE(recorded->GetCulling().HasCompleteGPUSceneCandidates(20u));

    const FakeBuffer* candidateBuffer = static_cast<const FakeBuffer*>(
        recorded->GetCulling().GetGPUSceneCandidateBuffer());
    ASSERT_NE(nullptr, candidateBuffer);
    EXPECT_EQ(candidate, ReadBufferValue<GPUSceneCullingCandidate>(*candidateBuffer, 0));
    EXPECT_EQ(secondCandidate,
              ReadBufferValue<GPUSceneCullingCandidate>(*candidateBuffer, 1));
    EXPECT_EQ(RHIContentValidity::Valid,
              recorded->GetAccessSnapshots().gpuSceneCandidates
                  .uniformAccess.contentValidity);

    const FakeBuffer* constantsBuffer = static_cast<const FakeBuffer*>(
        recorded->GetCulling().GetCullingConstantsBuffer());
    ASSERT_NE(nullptr, constantsBuffer);
    const GPUCullingConstants constants =
        ReadBufferValue<GPUCullingConstants>(*constantsBuffer);
    EXPECT_EQ(2u, constants.counts[0]);
    EXPECT_EQ(1u, constants.counts[1]);
    EXPECT_EQ(0u, constants.counts[2]);
    EXPECT_EQ(0u, constants.counts[3]);
    for (uint32 tableIndex = 0; tableIndex < 4; ++tableIndex)
    {
        EXPECT_EQ(capacities[tableIndex], constants.gpuSceneTableCounts0[tableIndex]);
    }
    EXPECT_EQ(capacities[4], constants.gpuSceneTableCounts1[0]);
    EXPECT_EQ(capacities[5], constants.gpuSceneTableCounts1[1]);
    EXPECT_EQ(0u, constants.gpuSceneTableCounts1[2]);
    EXPECT_EQ(0u, constants.gpuSceneTableCounts1[3]);
    EXPECT_EQ(RHIContentValidity::Valid,
              recorded->GetAccessSnapshots().constants.uniformAccess.contentValidity);

    const GPUSceneRasterResourceSnapshot rasterResources =
        recorded->GetGPUSceneRasterResourceSnapshot();
    ASSERT_TRUE(rasterResources.IsValid());
    EXPECT_EQ(candidateBuffer, rasterResources.GetCandidates());
    EXPECT_EQ(lease.buffers[static_cast<uint32>(GPUSceneResidentTable::Primitives)].Get(),
              rasterResources.GetPrimitives());
    EXPECT_EQ(lease.buffers[static_cast<uint32>(GPUSceneResidentTable::Transforms)].Get(),
              rasterResources.GetTransforms());
    EXPECT_EQ(2u, rasterResources.GetCandidateCount());
    EXPECT_EQ(config.maxInstances, rasterResources.GetCandidateCapacity());
    EXPECT_EQ(capacities[static_cast<uint32>(GPUSceneResidentTable::Primitives)],
              rasterResources.GetPrimitiveCapacity());
    EXPECT_EQ(capacities[static_cast<uint32>(GPUSceneResidentTable::Transforms)],
              rasterResources.GetTransformCapacity());
    EXPECT_EQ(19u, rasterResources.GetLeaseVersion());

    GPUSceneResidentGraphLease staleLease = lease;
    staleLease.version = 18u;
    EXPECT_TRUE(staleLease.IsValid());
    EXPECT_EQ(nullptr, culling.SealForGPUSceneGraph(identity, staleLease));

    GPUSceneResidentGraphLease zeroCapacityLease = lease;
    zeroCapacityLease.capacities[5] = 0u;
    EXPECT_TRUE(zeroCapacityLease.IsValid());
    EXPECT_EQ(nullptr, culling.SealForGPUSceneGraph(identity, zeroCapacityLease));

    GPUSceneResidentGraphLease oversizedLease = lease;
    oversizedLease.capacities[0] += 1u;
    EXPECT_TRUE(oversizedLease.IsValid());
    EXPECT_EQ(nullptr, culling.SealForGPUSceneGraph(identity, oversizedLease));

    GPUSceneResidentGraphLease wrongStrideLease = lease;
    RHIBufferDesc wrongStrideDesc;
    wrongStrideDesc.size =
        static_cast<uint64>(capacities[0]) * sizeof(GPUScenePrimitiveRow);
    wrongStrideDesc.usage = RHIBufferUsage::Structured | RHIBufferUsage::ShaderResource;
    wrongStrideDesc.memoryType = RHIMemoryType::Upload;
    wrongStrideDesc.stride = sizeof(uint32);
    wrongStrideDesc.debugName = "GPUDrivenValidation.GPUSceneWrongStride";
    wrongStrideLease.buffers[0] = device.CreateBuffer(wrongStrideDesc);
    EXPECT_TRUE(wrongStrideLease.IsValid());
    EXPECT_EQ(nullptr, culling.SealForGPUSceneGraph(identity, wrongStrideLease));

    GPUSceneResidentGraphLease invalidLease = lease;
    invalidLease.buffers[0].Reset();
    EXPECT_FALSE(invalidLease.IsValid());
    EXPECT_EQ(nullptr, culling.SealForGPUSceneGraph(identity, invalidLease));

    const std::shared_ptr<GPUCullingRecordedState> normalRecorded =
        culling.SealForGraph(identity);
    ASSERT_NE(nullptr, normalRecorded);
    EXPECT_FALSE(normalRecorded->GetGPUSceneRasterResourceSnapshot().IsValid());
    RenderSubmissionResourceBatch normalBatch;
    ASSERT_TRUE(normalRecorded->RetainSubmissionResources(normalBatch));
    const uint32 normalResourceCount = normalBatch.GetRetainedObjectCount();

    // Clearing the caller's refs before retention proves the sealed state
    // itself owns every table from the exact lease.
    lease.buffers = {};
    RenderSubmissionResourceBatch gpuSceneBatch;
    ASSERT_TRUE(recorded->RetainSubmissionResources(gpuSceneBatch));
    constexpr uint32 gpuSceneOnlyRetainedObjectCount = 16u;
    EXPECT_EQ(normalResourceCount + gpuSceneOnlyRetainedObjectCount,
              gpuSceneBatch.GetRetainedObjectCount());
    normalBatch.ReleaseUnsubmitted(retirement);
    gpuSceneBatch.ReleaseUnsubmitted(retirement);

    culling.InvalidateGPUSceneCandidates();
    EXPECT_FALSE(culling.HasCompleteGPUSceneCandidates());
    EXPECT_EQ(2u, culling.GetInstanceCount());
    EXPECT_TRUE(recorded->GetCulling().HasCompleteGPUSceneCandidates());
    EXPECT_TRUE(rasterResources.IsValid());
}

TEST_F(GPUDrivenValidationFixture,
       GPUSceneCullingDispatchesTwoComputePassesWithoutImplicitFallback)
{
    FakeDevice device;
    device.EnableTimelineRetirement();
    device.EnableGPUScenePipelineObjects();
    GPUCullingConfig config;
    config.maxInstances = 4;
    GPUCulling culling;
    culling.Initialize(&device, config);
    ASSERT_TRUE(culling.IsGPUSceneExecutionReady());

    culling.BeginFrame();
    ASSERT_EQ(0u, culling.AddInstance(
        MakeInstance(Vec3(0.0f, 0.0f, -5.0f), 1.0f, 36)));
    GPUSceneCullingCandidate candidate;
    candidate.primitiveSlot = 1u;
    candidate.primitiveGeneration = 7u;
    candidate.drawSlot = 1u;
    candidate.drawGeneration = 7u;
    candidate.objectIdLow = 42u;
    candidate.requiredPassMask = 2u;
    candidate.drawGroupIndex = 0u;
    candidate.drawGroupVisibleOffset = 0u;
    candidate.rasterInstanceIndex = 0u;
    candidate.materialParameterSlot = 3u;
    ASSERT_TRUE(culling.AddGPUSceneCandidate(candidate, 29u));
    culling.EndFrame();

    const std::array<uint32, GPU_SCENE_RESIDENT_TABLE_COUNT> capacities{
        8u, 9u, 10u, 11u, 12u, 13u};
    const GPUCullingRecordingIdentity identity{401u, 13u, 101u, 0u, 8u};
    const std::shared_ptr<GPUCullingRecordedState> gpuSceneRecorded =
        culling.SealForGPUSceneGraph(
            identity, MakeGPUSceneLease(device, 29u, capacities));
    ASSERT_NE(nullptr, gpuSceneRecorded);

    const Mat4 view = TestView();
    const Mat4 projection = TestProjection();
    FakeCommandContext gpuSceneContext;
    EXPECT_TRUE(gpuSceneRecorded->CullGPUScene(
        gpuSceneContext, view, projection));
    ASSERT_EQ(3u, gpuSceneContext.dispatches.size());
    EXPECT_EQ((std::array<uint32, 3>{1u, 1u, 1u}),
              gpuSceneContext.dispatches[0]);
    EXPECT_EQ((std::array<uint32, 3>{1u, 1u, 1u}),
              gpuSceneContext.dispatches[1]);
    EXPECT_EQ((std::array<uint32, 3>{1u, 1u, 1u}),
              gpuSceneContext.dispatches[2]);
    EXPECT_EQ(5u, gpuSceneContext.bufferBarriers.size());
    ASSERT_EQ(3u, gpuSceneContext.pipelines.size());
    ASSERT_EQ(3u, gpuSceneContext.descriptorSets.size());
    EXPECT_NE(gpuSceneContext.pipelines[0], gpuSceneContext.pipelines[1]);
    EXPECT_NE(gpuSceneContext.pipelines[1], gpuSceneContext.pipelines[2]);
    EXPECT_EQ(gpuSceneContext.descriptorSets[0],
              gpuSceneContext.descriptorSets[1]);
    EXPECT_EQ(gpuSceneContext.descriptorSets[1],
              gpuSceneContext.descriptorSets[2]);
    EXPECT_TRUE(gpuSceneRecorded->GetCulling().WasGpuExecutionUsedLastCull());
    EXPECT_FALSE(gpuSceneRecorded->GetCulling().WasCpuFallbackUsedLastCull());
    EXPECT_TRUE(gpuSceneRecorded->GetCulling().GetVisibleInstanceIndices().empty());

    const FakeBuffer* constantsBuffer = static_cast<const FakeBuffer*>(
        gpuSceneRecorded->GetCulling().GetCullingConstantsBuffer());
    ASSERT_NE(nullptr, constantsBuffer);
    const GPUCullingConstants constants =
        ReadBufferValue<GPUCullingConstants>(*constantsBuffer);
    const Mat4 expectedViewProjection = projection * view;
    for (uint32 column = 0; column < 4; ++column)
    {
        for (uint32 row = 0; row < 4; ++row)
        {
            EXPECT_FLOAT_EQ(expectedViewProjection[column][row],
                            constants.viewProj[column][row]);
        }
    }
    EXPECT_FLOAT_EQ(config.maxDrawDistance, constants.params.x);
    EXPECT_FLOAT_EQ(1.0f, constants.params.z);
    EXPECT_FLOAT_EQ(1.0f, constants.params.w);
    EXPECT_EQ(1u, constants.counts[0]);
    EXPECT_EQ(1u, constants.counts[1]);
    for (uint32 tableIndex = 0; tableIndex < 4; ++tableIndex)
    {
        EXPECT_EQ(capacities[tableIndex], constants.gpuSceneTableCounts0[tableIndex]);
    }
    EXPECT_EQ(capacities[4], constants.gpuSceneTableCounts1[0]);
    EXPECT_EQ(capacities[5], constants.gpuSceneTableCounts1[1]);

    auto* mutableConstantsBuffer = static_cast<FakeBuffer*>(
        gpuSceneRecorded->GetCulling().GetCullingConstantsBuffer());
    ASSERT_NE(nullptr, mutableConstantsBuffer);
    mutableConstantsBuffer->SetMapSucceeds(false);
    device.failTransientUploadMap = true;
    FakeCommandContext failedGPUSceneContext;
    EXPECT_FALSE(gpuSceneRecorded->CullGPUScene(
        failedGPUSceneContext, view, projection));
    EXPECT_TRUE(failedGPUSceneContext.dispatches.empty());
    EXPECT_FALSE(gpuSceneRecorded->GetCulling().WasGpuExecutionUsedLastCull());
    EXPECT_FALSE(gpuSceneRecorded->GetCulling().WasCpuFallbackUsedLastCull());
    device.failTransientUploadMap = false;
    mutableConstantsBuffer->SetMapSucceeds(true);

    const std::shared_ptr<GPUCullingRecordedState> normalRecorded =
        culling.SealForGraph(identity);
    ASSERT_NE(nullptr, normalRecorded);
    FakeCommandContext rejectedGPUSceneContext;
    EXPECT_FALSE(normalRecorded->CullGPUScene(
        rejectedGPUSceneContext, view, projection));
    EXPECT_TRUE(rejectedGPUSceneContext.dispatches.empty());
    EXPECT_FALSE(normalRecorded->GetCulling().WasGpuExecutionUsedLastCull());
    EXPECT_FALSE(normalRecorded->GetCulling().WasCpuFallbackUsedLastCull());

    // The caller-selected normal seal remains available as the per-pass
    // fallback; the failed GPU-scene recording never invoked it implicitly.
    FakeCommandContext normalContext;
    normalRecorded->Cull(normalContext, view, projection);
    EXPECT_EQ(3u, normalContext.dispatches.size());
    ASSERT_EQ(3u, normalContext.pipelines.size());
    ASSERT_EQ(3u, normalContext.descriptorSets.size());
    EXPECT_NE(gpuSceneContext.pipelines[0], normalContext.pipelines[0]);
    EXPECT_NE(gpuSceneContext.pipelines[1], normalContext.pipelines[1]);
    EXPECT_NE(gpuSceneContext.descriptorSets[0], normalContext.descriptorSets[0]);
    EXPECT_TRUE(normalRecorded->GetCulling().WasGpuExecutionUsedLastCull());
}

TEST_F(GPUDrivenValidationFixture,
       SceneRendererGPUSceneLeaseWiringUsesOneAcquireAndPreflightsBeforeGraphMutation)
{
    const std::filesystem::path root = FindWorkspaceRoot();
    ASSERT_FALSE(root.empty());
    const std::string source = ReadTextFile(
        root / "Render" / "Private" / "Renderer" / "SceneRenderer.cpp");
    const std::string uploaderHeader = ReadTextFile(
        root / "Render" / "Private" / "GPUScene" / "GPUSceneUploader.h");
    const std::string subsystemSource = ReadTextFile(
        root / "Render" / "Private" / "RenderSubsystem.cpp");
    ASSERT_FALSE(source.empty());
    ASSERT_FALSE(uploaderHeader.empty());
    ASSERT_FALSE(subsystemSource.empty());

    size_t acquireCount = 0;
    size_t acquireOffset = source.find("AcquireCurrentGraphLease(");
    while (acquireOffset != std::string::npos)
    {
        ++acquireCount;
        acquireOffset = source.find("AcquireCurrentGraphLease(", acquireOffset + 1);
    }
    EXPECT_EQ(1u, acquireCount);
    EXPECT_NE(source.find("m_gpuSceneUploader->BuildRenderGraph("),
              std::string::npos);
    EXPECT_NE(source.find("requestedGPUSceneTier"), std::string::npos);
    EXPECT_NE(source.find("useGPUSceneTier"), std::string::npos);
    EXPECT_NE(source.find("preflightGPUScenePass"), std::string::npos);
    EXPECT_NE(source.find("owner->SealForGPUSceneGraph("), std::string::npos);
    EXPECT_NE(source.find("HasCompleteGPUSceneCandidates(requiredResidentVersion)"),
              std::string::npos);
    EXPECT_NE(source.find("owner->SealForGraph(cullingIdentity)"),
              std::string::npos);
    EXPECT_NE(source.find("m_gpuSceneUploader->CancelCurrentGraphLease()"),
              std::string::npos);
    EXPECT_NE(source.find("data.gpuSceneCandidates = builder.Read("),
              std::string::npos);
    EXPECT_NE(source.find("data.gpuSceneTables[tableIndex] = builder.Read("),
              std::string::npos);
    EXPECT_NE(source.find("recordedState->CullGPUScene("), std::string::npos);
    EXPECT_NE(source.find("if (!recordedState->CullGPUScene("),
              std::string::npos);
    EXPECT_NE(source.find("m_gpuSceneCullingCommandRecordingFailed"),
              std::string::npos);
    EXPECT_NE(source.find("GPU-scene culling command recording failed"),
              std::string::npos);
    EXPECT_NE(source.find("m_gpuSceneUpdate->ResolveAcceptedDraw("),
              std::string::npos);
    EXPECT_NE(source.find("owner->InvalidateGPUSceneCandidates()"),
              std::string::npos);
    EXPECT_NE(uploaderHeader.find("CancelCurrentGraphLease"),
              std::string::npos);

    const size_t renderStart = source.find("void SceneRenderer::Render()");
    const size_t graphExecute = source.find("m_renderGraph->Execute(*ctx);", renderStart);
    const size_t failedFrameGate = source.find(
        "graphExecuted = !m_gpuSceneCullingCommandRecordingFailed &&", graphExecute);
    const size_t uploaderCommit = source.find(
        "m_gpuSceneUploader->CommitRealizedAccess(*m_renderGraph);", graphExecute);
    const size_t cullingCommit = source.find(
        "CommitGPUDrivenAccessSnapshots();", graphExecute);
    ASSERT_NE(std::string::npos, renderStart);
    ASSERT_NE(std::string::npos, graphExecute);
    ASSERT_NE(std::string::npos, failedFrameGate);
    ASSERT_NE(std::string::npos, uploaderCommit);
    ASSERT_NE(std::string::npos, cullingCommit);
    EXPECT_LT(graphExecute, failedFrameGate);
    EXPECT_LT(failedFrameGate, uploaderCommit);
    EXPECT_LT(failedFrameGate, cullingCommit);
    const size_t commitSuccessGuard = source.rfind(
        "if (graphExecuted)", uploaderCommit);
    ASSERT_NE(std::string::npos, commitSuccessGuard);
    EXPECT_LT(failedFrameGate, commitSuccessGuard);
    EXPECT_LT(commitSuccessGuard, uploaderCommit);

    const size_t acceptedRendererFrame = source.find(
        "RenderFrameExecutionResult SceneRenderer::RenderAcceptedFrame()");
    const size_t rendererRenderCall = source.find("Render();", acceptedRendererFrame);
    const size_t rendererFailureReturn = source.find(
        "if (HasSubmissionFailure())", rendererRenderCall);
    const size_t cullingRetain = source.find(
        "m_depthGPUCulling->RetainSubmissionResources", rendererRenderCall);
    ASSERT_NE(std::string::npos, acceptedRendererFrame);
    ASSERT_NE(std::string::npos, rendererRenderCall);
    ASSERT_NE(std::string::npos, rendererFailureReturn);
    ASSERT_NE(std::string::npos, cullingRetain);
    EXPECT_LT(rendererRenderCall, rendererFailureReturn);
    EXPECT_LT(rendererFailureReturn, cullingRetain);

    const size_t acceptedFrame = subsystemSource.find("m_sceneRenderer->RenderAcceptedFrame()");
    const size_t releaseUnsubmitted = subsystemSource.find(
        "m_sceneRenderer->ReleaseUnsubmittedFrame();", acceptedFrame);
    const size_t abortFrame = subsystemSource.find("m_context->AbortFrame();", acceptedFrame);
    const size_t submitFrame = subsystemSource.find("m_context->EndFrame();", acceptedFrame);
    ASSERT_NE(std::string::npos, acceptedFrame);
    ASSERT_NE(std::string::npos, releaseUnsubmitted);
    ASSERT_NE(std::string::npos, abortFrame);
    ASSERT_NE(std::string::npos, submitFrame);
    EXPECT_LT(acceptedFrame, releaseUnsubmitted);
    EXPECT_LT(abortFrame, releaseUnsubmitted);
    EXPECT_LT(releaseUnsubmitted, submitFrame);
}

TEST_F(GPUDrivenValidationFixture,
       GPUSceneRasterPassesUseSealedBindingsAndSupportedDepthInputs)
{
    const std::filesystem::path root = FindWorkspaceRoot();
    ASSERT_FALSE(root.empty());
    const std::string renderer = ReadTextFile(
        root / "Render" / "Private" / "Renderer" / "SceneRenderer.cpp");
    const std::string recordContext = ReadTextFile(
        root / "Render" / "Include" / "Render" / "Passes" /
        "RenderPassRecordContext.h");
    const std::string depthPass = ReadTextFile(
        root / "Render" / "Private" / "Passes" / "DepthPrepass.cpp");
    const std::string opaquePass = ReadTextFile(
        root / "Render" / "Private" / "Passes" / "OpaquePass.cpp");
    ASSERT_FALSE(renderer.empty());
    ASSERT_FALSE(recordContext.empty());
    ASSERT_FALSE(depthPass.empty());
    ASSERT_FALSE(opaquePass.empty());

    const size_t gpuSceneSeal = renderer.find("owner->SealForGPUSceneGraph(");
    const size_t bindingCreate = renderer.find(
        "CreateGPUSceneRasterBindingSnapshot(", gpuSceneSeal);
    const size_t graphImport = renderer.find("m_renderGraph->ImportBuffer(", gpuSceneSeal);
    ASSERT_NE(std::string::npos, gpuSceneSeal);
    ASSERT_NE(std::string::npos, bindingCreate);
    ASSERT_NE(std::string::npos, graphImport);
    EXPECT_LT(gpuSceneSeal, bindingCreate);
    EXPECT_LT(bindingCreate, graphImport);
    EXPECT_NE(std::string::npos,
              renderer.find("retainedBinding->RetainSubmissionResources"));
    EXPECT_NE(std::string::npos,
              renderer.find("m_gpuSceneRasterCommandRecordingFailed"));

    EXPECT_NE(std::string::npos,
              recordContext.find("gpuSceneCandidates"));
    EXPECT_NE(std::string::npos,
              recordContext.find("gpuScenePrimitives"));
    EXPECT_NE(std::string::npos,
              recordContext.find("gpuSceneTransforms"));
    EXPECT_NE(std::string::npos,
              recordContext.find("gpuSceneRasterBinding"));
    EXPECT_NE(std::string::npos,
              recordContext.find("gpuSceneLeaseVersion"));

    for (const std::string* pass : {&depthPass, &opaquePass})
    {
        EXPECT_NE(std::string::npos,
                  pass->find("builder.Read(m_gpuSceneCandidateHandle, RHIShaderStage::Vertex)"));
        EXPECT_NE(std::string::npos,
                  pass->find("builder.Read(m_gpuScenePrimitiveHandle, RHIShaderStage::Vertex)"));
        EXPECT_NE(std::string::npos,
                  pass->find("builder.Read(m_gpuSceneTransformHandle, RHIShaderStage::Vertex)"));
        EXPECT_NE(std::string::npos,
                  pass->find("gpuSceneObjectOffsets{0u}"));
    }

    EXPECT_NE(std::string::npos,
              depthPass.find("group.pipelineVariant != MaterialPipelineVariant::Opaque"));
}

TEST_F(GPUDrivenValidationFixture,
       GPUSceneCullingShaderUsesSharedSixTableAbiAndExactGenerationValidation)
{
    const std::filesystem::path root = FindWorkspaceRoot();
    ASSERT_FALSE(root.empty());

    const std::string header = ReadTextFile(
        root / "Render" / "Include" / "Render" / "GPUDriven" / "GPUCulling.h");
    const std::string source = ReadTextFile(
        root / "Render" / "Private" / "GPUDriven" / "GPUCulling.cpp");
    const std::string sharedShader = ReadTextFile(
        root / "Render" / "Shaders" / "GPUDriven" / "GPUSceneCulling.hlsli");
    const std::string shader = ReadTextFile(
        root / "Render" / "Shaders" / "GPUDriven" / "GPUSceneCulling.hlsl");
    const std::string normalShader = ReadTextFile(
        root / "Render" / "Shaders" / "GPUDriven" / "GPUCulling.hlsl");
    ASSERT_FALSE(header.empty());
    ASSERT_FALSE(source.empty());
    ASSERT_FALSE(sharedShader.empty());
    ASSERT_FALSE(shader.empty());
    ASSERT_FALSE(normalShader.empty());

    EXPECT_NE(header.find("sizeof(GPUSceneCullingCandidate) == 48"),
              std::string::npos);
    EXPECT_NE(header.find("RVX_GPU_SCENE_CULLING_TABLE_COUNT = 6"),
              std::string::npos);
    EXPECT_NE(header.find("SealForGPUSceneGraph"), std::string::npos);
    EXPECT_NE(source.find("ConfigureGPUSceneRecording"), std::string::npos);
    EXPECT_NE(source.find("FindGPUSceneCullingShaderPath"), std::string::npos);
    EXPECT_NE(source.find("BindBuffer(6 + tableIndex"), std::string::npos);
    EXPECT_NE(source.find("m_gpuSceneTableBuffers = lease.buffers"),
              std::string::npos);

    EXPECT_NE(sharedShader.find("struct GPUScenePrimitiveRow"), std::string::npos);
    EXPECT_NE(sharedShader.find("struct GPUSceneBoundsRow"), std::string::npos);
    EXPECT_NE(sharedShader.find("struct GPUSceneTransformRow"), std::string::npos);
    EXPECT_NE(sharedShader.find("struct GPUSceneMaterialRow"), std::string::npos);
    EXPECT_NE(sharedShader.find("struct GPUSceneGeometryRow"), std::string::npos);
    EXPECT_NE(sharedShader.find("struct GPUSceneDrawMetadataRow"), std::string::npos);
    EXPECT_NE(sharedShader.find("GPUSceneValidateCandidateRows"), std::string::npos);
    EXPECT_NE(sharedShader.find("drawRef.y != primitive.firstDraw.y"),
              std::string::npos);
    EXPECT_NE(sharedShader.find("draw.instanceCount != 1u"), std::string::npos);
    EXPECT_NE(sharedShader.find("draw.firstInstance != 0u"), std::string::npos);
    EXPECT_NE(sharedShader.find("candidate.requiredPassMask &"),
              std::string::npos);
    EXPECT_NE(sharedShader.find("RVX_GPU_SCENE_ROW_FLAG_TOMBSTONE"),
              std::string::npos);
    EXPECT_NE(shader.find("#include \"GPUSceneCulling.hlsli\""),
              std::string::npos);
    EXPECT_NE(shader.find("void CSGPUSceneFrustumCull"), std::string::npos);
    EXPECT_NE(shader.find("void CSGPUSceneCompactDraws"), std::string::npos);
    EXPECT_NE(shader.find("void CSGPUSceneFinalizeDrawGroups"),
              std::string::npos);
    EXPECT_NE(shader.find("uint4 Counts"), std::string::npos);
    EXPECT_EQ(shader.find("float4 Counts"), std::string::npos);
    EXPECT_EQ(shader.find("(uint)Counts"), std::string::npos);
    EXPECT_NE(normalShader.find("uint4 Counts"), std::string::npos);
    EXPECT_EQ(normalShader.find("float4 Counts"), std::string::npos);
}

TEST_F(GPUDrivenValidationFixture,
       NormalCullingWritesTheCompleteFixedConstantsAbiWithoutLeaseResidue)
{
    FakeDevice device;
    GPUCullingConfig config;
    config.maxInstances = 4;
    GPUCulling culling;
    culling.Initialize(&device, config);

    culling.BeginFrame();
    ASSERT_EQ(0u, culling.AddInstance(
        MakeInstance(Vec3(0.0f, 0.0f, -5.0f), 1.0f, 36)));
    culling.EndFrame();

    FakeCommandContext context;
    culling.Cull(context, TestView(), TestProjection());

    const FakeBuffer* constantsBuffer = static_cast<const FakeBuffer*>(
        culling.GetCullingConstantsBuffer());
    ASSERT_NE(nullptr, constantsBuffer);
    const GPUCullingConstants constants =
        ReadBufferValue<GPUCullingConstants>(*constantsBuffer);
    EXPECT_EQ(1u, constants.counts[0]);
    EXPECT_EQ(1u, constants.counts[1]);
    EXPECT_EQ(0u, constants.counts[2]);
    EXPECT_EQ(0u, constants.counts[3]);
    for (uint32 tableIndex = 0; tableIndex < 4; ++tableIndex)
    {
        EXPECT_EQ(0u, constants.gpuSceneTableCounts0[tableIndex]);
        EXPECT_EQ(0u, constants.gpuSceneTableCounts1[tableIndex]);
    }
    EXPECT_EQ(RHIContentValidity::Valid,
              culling.GetAccessSnapshots().constants.uniformAccess.contentValidity);
}

TEST_F(GPUDrivenValidationFixture, GPUSceneCullingShaderEntriesCompileForDX12)
{
    const std::filesystem::path root = FindWorkspaceRoot();
    ASSERT_FALSE(root.empty());

    const std::filesystem::path shaderPath =
        root / "Render" / "Shaders" / "GPUDriven" / "GPUSceneCulling.hlsl";
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

    options.entryPoint = "CSGPUSceneFrustumCull";
    ShaderCompileResult frustumResult = compiler->Compile(options);
    ASSERT_TRUE(frustumResult.success) << frustumResult.errorMessage;
    EXPECT_FALSE(frustumResult.bytecode.empty());

    options.entryPoint = "CSGPUSceneCompactDraws";
    ShaderCompileResult compactResult = compiler->Compile(options);
    ASSERT_TRUE(compactResult.success) << compactResult.errorMessage;
    EXPECT_FALSE(compactResult.bytecode.empty());

    options.entryPoint = "CSGPUSceneFinalizeDrawGroups";
    ShaderCompileResult finalizeResult = compiler->Compile(options);
    ASSERT_TRUE(finalizeResult.success) << finalizeResult.errorMessage;
    EXPECT_FALSE(finalizeResult.bytecode.empty());
}

TEST_F(GPUDrivenValidationFixture, GPUDrivenVertexShaderEntriesCompileForDX12SM60)
{
    const std::filesystem::path root = FindWorkspaceRoot();
    ASSERT_FALSE(root.empty());

    std::unique_ptr<IShaderCompiler> compiler = CreateShaderCompiler();
    ASSERT_NE(nullptr, compiler);

    const auto compileVertexEntry = [&compiler](const std::filesystem::path& shaderPath)
    {
        const std::string shader = ReadTextFile(shaderPath);
        ASSERT_FALSE(shader.empty());

        const std::string shaderPathString = shaderPath.string();
        ShaderCompileOptions options;
        options.stage = RHIShaderStage::Vertex;
        options.sourceCode = shader.c_str();
        options.sourcePath = shaderPathString.c_str();
        options.targetBackend = RHIBackendType::DX12;
        options.targetProfile = "vs_6_0";
        options.entryPoint = "VSMainGPUDriven";
        options.enableDebugInfo = false;
        options.enableOptimization = true;

        const ShaderCompileSupport support = compiler->QuerySupport(options);
        if (!support.IsSupported())
        {
            GTEST_SKIP() << support.reason;
        }

        const ShaderCompileResult result = compiler->Compile(options);
        ASSERT_TRUE(result.success) << result.errorMessage;
        EXPECT_FALSE(result.bytecode.empty());
    };

    compileVertexEntry(root / "Render" / "Shaders" / "DefaultLit.hlsl");
    compileVertexEntry(root / "Render" / "Shaders" / "DepthOnly.hlsl");

    const auto compileGPUSceneVertexEntry = [&compiler](
        const std::filesystem::path& shaderPath,
        const char* entryPoint)
    {
        const std::string shader = ReadTextFile(shaderPath);
        ASSERT_FALSE(shader.empty());

        const std::string shaderPathString = shaderPath.string();
        ShaderCompileOptions options;
        options.stage = RHIShaderStage::Vertex;
        options.sourceCode = shader.c_str();
        options.sourcePath = shaderPathString.c_str();
        options.targetBackend = RHIBackendType::DX12;
        options.targetProfile = "vs_6_0";
        options.entryPoint = entryPoint;
        options.defines.push_back({"RVX_GPU_SCENE_RASTER", "1"});
        options.enableDebugInfo = false;
        options.enableOptimization = true;

        const ShaderCompileSupport support = compiler->QuerySupport(options);
        if (!support.IsSupported())
        {
            GTEST_SKIP() << support.reason;
        }

        const ShaderCompileResult result = compiler->Compile(options);
        ASSERT_TRUE(result.success) << result.errorMessage;
        EXPECT_FALSE(result.bytecode.empty());
        EXPECT_FALSE(result.sourceInfo.IsEmpty());

        const auto containsInclude = [&result](const char* filename)
        {
            return std::any_of(
                result.sourceInfo.includeFiles.begin(), result.sourceInfo.includeFiles.end(),
                [filename](const std::string& includePath)
                {
                    return std::filesystem::path(includePath).filename() == filename;
                });
        };
        EXPECT_TRUE(containsInclude("GPUSceneRaster.hlsli"));
        EXPECT_TRUE(containsInclude("GPUSceneCulling.hlsli"));
    };

    compileGPUSceneVertexEntry(
        root / "Render" / "Shaders" / "DefaultLit.hlsl", "VSMainGPUScene");
    compileGPUSceneVertexEntry(
        root / "Render" / "Shaders" / "DefaultLit.hlsl",
        "VSMainGPUSceneInstancedMaterial");
    compileGPUSceneVertexEntry(
        root / "Render" / "Shaders" / "DepthOnly.hlsl", "VSMainGPUScene");
}

TEST_F(GPUDrivenValidationFixture,
       GPUSceneRasterShaderUsesExactRowMajorTransformAndFailClosedResolution)
{
    const std::filesystem::path root = FindWorkspaceRoot();
    ASSERT_FALSE(root.empty());
    const std::string rasterInclude = ReadTextFile(
        root / "Render" / "Shaders" / "GPUDriven" / "GPUSceneRaster.hlsli");
    const std::string sharedSchema = ReadTextFile(
        root / "Render" / "Shaders" / "GPUDriven" / "GPUSceneCulling.hlsli");
    const std::string cpuSchema = ReadTextFile(
        root / "Render" / "Include" / "Render" / "GPUScene" /
        "GPUSceneSchema.h");
    const std::string defaultLit = ReadTextFile(
        root / "Render" / "Shaders" / "DefaultLit.hlsl");
    const std::string depthOnly = ReadTextFile(
        root / "Render" / "Shaders" / "DepthOnly.hlsl");
    ASSERT_FALSE(rasterInclude.empty());
    ASSERT_FALSE(sharedSchema.empty());
    ASSERT_FALSE(cpuSchema.empty());
    ASSERT_FALSE(defaultLit.empty());
    ASSERT_FALSE(depthOnly.empty());

    EXPECT_NE(rasterInclude.find("candidate.rasterInstanceIndex != rasterInstanceIndex"),
              std::string::npos);
    EXPECT_NE(rasterInclude.find("GPUSceneIsLiveHeader(primitive.header"),
              std::string::npos);
    EXPECT_NE(rasterInclude.find("GPUSceneIsLiveHeader(transform.header"),
              std::string::npos);
    EXPECT_NE(sharedSchema.find("RVX_GPU_SCENE_ROW_FLAG_TOMBSTONE"),
              std::string::npos);
    EXPECT_NE(rasterInclude.find("dot(transform.worldFromLocal[0]"),
              std::string::npos);
    EXPECT_NE(rasterInclude.find("dot(transform.normalFromLocal[0].xyz"),
              std::string::npos);
    EXPECT_NE(rasterInclude.find("GPUSceneInvalidClipPosition"), std::string::npos);
    EXPECT_NE(rasterInclude.find("out uint primitiveFlags"), std::string::npos);
    EXPECT_NE(rasterInclude.find("RVX_GPU_SCENE_PRIMITIVE_RECEIVES_SHADOW (1u << 2u)"),
              std::string::npos);
    EXPECT_NE(cpuSchema.find("enum class GPUScenePrimitiveFlags : uint32"),
              std::string::npos);
    EXPECT_NE(cpuSchema.find("ReceivesShadow = 1U << 2U"), std::string::npos);
    EXPECT_NE(cpuSchema.find("HasGPUScenePrimitiveFlag"), std::string::npos);
    EXPECT_NE(defaultLit.find("PSInput VSMainGPUScene"), std::string::npos);
    EXPECT_NE(depthOnly.find("VSOutput VSMainGPUScene"), std::string::npos);
    EXPECT_NE(defaultLit.find("output.WorldTangent = float4(0.0f"), std::string::npos);
    EXPECT_NE(defaultLit.find("GPUSceneTransformNormal"), std::string::npos);
    EXPECT_NE(defaultLit.find("GPUSceneTransformTangent"), std::string::npos);
    EXPECT_NE(defaultLit.find("nointerpolation float ReceivesShadowValue : TEXCOORD4"),
              std::string::npos);
    EXPECT_NE(defaultLit.find("primitiveFlags & RVX_GPU_SCENE_PRIMITIVE_RECEIVES_SHADOW"),
              std::string::npos);
    EXPECT_NE(defaultLit.find("input.ReceivesShadowValue > 0.5"),
              std::string::npos);

    constexpr uint32 receivesShadow =
        static_cast<uint32>(GPUScenePrimitiveFlags::ReceivesShadow);
    const uint32 mixedPrimitiveFlags[] = {receivesShadow, 0u};
    EXPECT_TRUE(HasGPUScenePrimitiveFlag(
        mixedPrimitiveFlags[0], GPUScenePrimitiveFlags::ReceivesShadow));
    EXPECT_FALSE(HasGPUScenePrimitiveFlag(
        mixedPrimitiveFlags[1], GPUScenePrimitiveFlags::ReceivesShadow));

    const auto assertGPUSceneHelperIsPermutationOnly =
        [](const std::string& shaderSource)
    {
        const size_t helperInclude = shaderSource.find(
            "#include \"GPUDriven/GPUSceneRaster.hlsli\"");
        ASSERT_NE(std::string::npos, helperInclude);
        const size_t includeGuard = shaderSource.rfind(
            "#if defined(RVX_GPU_SCENE_RASTER)", helperInclude);
        ASSERT_NE(std::string::npos, includeGuard);
        EXPECT_LT(includeGuard, helperInclude);
        EXPECT_NE(std::string::npos, shaderSource.find("#endif", helperInclude));

        const size_t gpuSceneEntry = shaderSource.find("VSMainGPUScene");
        ASSERT_NE(std::string::npos, gpuSceneEntry);
        const size_t entryGuard = shaderSource.rfind(
            "#if defined(RVX_GPU_SCENE_RASTER)", gpuSceneEntry);
        ASSERT_NE(std::string::npos, entryGuard);
        EXPECT_LT(entryGuard, gpuSceneEntry);
        EXPECT_EQ(std::string::npos,
                  shaderSource.substr(0, entryGuard).find(
                      "GPUSceneResolveRasterTransform"));
    };
    assertGPUSceneHelperIsPermutationOnly(defaultLit);
    assertGPUSceneHelperIsPermutationOnly(depthOnly);

    // GPUSceneUpdate packs GLM's column-major Mat4 into explicit rows.  The
    // row-dot contract below therefore matches Tier1's matrix * vector path,
    // including a non-uniform normal transform.
    Mat4 world = Mat4Identity();
    world[0][0] = 2.0f;
    world[1][1] = 3.0f;
    world[2][2] = 4.0f;
    world[3][0] = 5.0f;
    world[3][1] = -2.0f;
    world[3][2] = 7.0f;
    const Vec4 position(1.5f, -2.0f, 0.25f, 1.0f);
    const Vec4 tierOnePosition = world * position;
    const Vec4 gpuScenePosition(
        world[0][0] * position.x + world[1][0] * position.y +
            world[2][0] * position.z + world[3][0] * position.w,
        world[0][1] * position.x + world[1][1] * position.y +
            world[2][1] * position.z + world[3][1] * position.w,
        world[0][2] * position.x + world[1][2] * position.y +
            world[2][2] * position.z + world[3][2] * position.w,
        1.0f);
    EXPECT_FLOAT_EQ(tierOnePosition.x, gpuScenePosition.x);
    EXPECT_FLOAT_EQ(tierOnePosition.y, gpuScenePosition.y);
    EXPECT_FLOAT_EQ(tierOnePosition.z, gpuScenePosition.z);
    EXPECT_FLOAT_EQ(tierOnePosition.w, gpuScenePosition.w);

    Mat4 normal = Mat4Identity();
    normal[0][0] = 0.5f;
    normal[1][1] = 1.0f / 3.0f;
    normal[2][2] = 0.25f;
    const Vec4 inputNormal(0.25f, 0.5f, -0.75f, 0.0f);
    const Vec4 tierOneNormal = normal * inputNormal;
    const Vec3 gpuSceneNormal(
        normal[0][0] * inputNormal.x + normal[1][0] * inputNormal.y +
            normal[2][0] * inputNormal.z,
        normal[0][1] * inputNormal.x + normal[1][1] * inputNormal.y +
            normal[2][1] * inputNormal.z,
        normal[0][2] * inputNormal.x + normal[1][2] * inputNormal.y +
            normal[2][2] * inputNormal.z);
    EXPECT_FLOAT_EQ(tierOneNormal.x, gpuSceneNormal.x);
    EXPECT_FLOAT_EQ(tierOneNormal.y, gpuSceneNormal.y);
    EXPECT_FLOAT_EQ(tierOneNormal.z, gpuSceneNormal.z);
}
