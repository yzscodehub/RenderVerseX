#include "Core/Log.h"
#include "Render/GPUDriven/GPUCulling.h"
#include "Render/GPUDriven/GPUDrivenDiagnostics.h"
#include "Render/GPUDriven/GPUDrivenPolicy.h"
#include "Render/Renderer/RenderDrawItem.h"
#include "Render/Renderer/RenderScene.h"
#include "RHI/RHI.h"
#include "RHI/RHICommandContext.h"
#include "ShaderCompiler/ShaderCompiler.h"

#include <gtest/gtest.h>

#include <array>
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

        uint32 drawIndexedIndirectCalls = 0;
        RHIBuffer* lastIndirectBuffer = nullptr;
        uint64 lastIndirectOffset = 0;
        uint32 lastIndirectDrawCount = 0;
        uint32 lastIndirectStride = 0;
        std::vector<RHIBufferBarrier> bufferBarriers;
        std::vector<std::array<uint32, 3>> dispatches;
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

TEST_F(GPUDrivenValidationFixture, NonDX12BackendsRemainUnqualified)
{
    for (RHIBackendType backend : {
             RHIBackendType::Vulkan,
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
    input.supportsIndirectDrawCount = true;
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
    input.supportsIndirectDrawCount = true;
    input.pipelineReady = true;

    GPUDrivenPolicyDecision decision = ResolveGPUDrivenPolicy(input);
    EXPECT_TRUE(decision.enabled);
    EXPECT_EQ(GPUDrivenPolicyReason::None, decision.reason);

    input.supportsIndirectDrawCount = false;
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
    input.supportsIndirectDrawCount = true;
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
    input.supportsIndirectDrawCount = true;
    input.pipelineReady = true;

    const GPUDrivenPolicyDecision decision = ResolveGPUDrivenPolicy(input);
    EXPECT_FALSE(decision.enabled);
    EXPECT_EQ(GPUDrivenPolicyReason::InvalidMode, decision.reason);
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
    EXPECT_NE(header.find("SetGPUDrivenCullingMode"), std::string::npos);
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
    EXPECT_NE(source.find("MakeRHIAccessSnapshot(RHIResourceState::ConstantBuffer"), std::string::npos);
    EXPECT_NE(source.find("MakeRHIAccessSnapshot(RHIResourceState::ShaderResource"), std::string::npos);
    EXPECT_NE(source.find("RHIResourceState::UnorderedAccess,"), std::string::npos);
    EXPECT_NE(source.find("data.indirectDraws = builder.Write(data.indirectDraws, unorderedAccess)"),
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
    EXPECT_NE(source.find("m_gpuDrivenCullingStats.executionDecision = m_gpuCulling->GetExecutionDecision();"),
              std::string::npos);
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
    EXPECT_NE(source.find("GPUCulling.InstanceIndexBuffer"), std::string::npos);
    EXPECT_NE(source.find("RHIBufferUsage::Vertex"), std::string::npos);
    EXPECT_NE(source.find("(static_cast<uint64>(m_config.maxInstances) + 1u)"),
              std::string::npos);
    EXPECT_NE(source.find("uint32 groupCount = (m_instanceCount / 64) + 1;"),
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
    const std::string depthOnly =
        ReadTextFile(root / "Render" / "Shaders" / "DepthOnly.hlsl");
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
    EXPECT_NE(defaultLit.find("uint InstanceIndex : INSTANCE_INDEX"), std::string::npos);
    EXPECT_NE(defaultLit.find("GPUDrivenInstances[input.InstanceIndex]"), std::string::npos);
    EXPECT_EQ(defaultLit.find("GPUDrivenInstances[instanceId]"), std::string::npos);
    EXPECT_NE(depthOnly.find("uint InstanceIndex : INSTANCE_INDEX"), std::string::npos);
    EXPECT_NE(depthOnly.find("GPUDrivenInstances[input.InstanceIndex]"), std::string::npos);
    EXPECT_EQ(depthOnly.find("GPUDrivenInstances[instanceId]"), std::string::npos);
    EXPECT_NE(pipelineHeader.find("GetGPUDrivenPipelineForVariant"), std::string::npos);
    EXPECT_NE(pipelineSource.find("GPUDrivenOpaquePipeline"), std::string::npos);
    EXPECT_NE(pipelineSource.find("RVX_PIPELINE_PURPOSE_GPU_DRIVEN_DEFAULT"), std::string::npos);
    EXPECT_NE(pipelineSource.find("AddElement(\"INSTANCE_INDEX\", RHIFormat::R32_UINT, 6)"),
              std::string::npos);
    EXPECT_NE(pipelineSource.find("instanceIndexElement.perInstance = true"), std::string::npos);
    EXPECT_NE(pipelineSource.find("instanceIndexElement.instanceDataStepRate = 1"), std::string::npos);
    EXPECT_NE(opaqueHeader.find("SetGPUDrivenCullingSource"), std::string::npos);
    EXPECT_NE(opaqueHeader.find("SetGPUDrivenRenderGraphResources"), std::string::npos);
    EXPECT_NE(opaqueSource.find("TryDrawGPUDrivenIndirect"), std::string::npos);
    EXPECT_NE(opaqueSource.find("DrawIndexedIndirectGroup"), std::string::npos);
    EXPECT_NE(opaqueSource.find("ctx.SetVertexBuffer(6, m_gpuCulling->GetInstanceIndexBuffer())"),
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
    EXPECT_NE(modelViewer.find("stats.graphInputDrawItemCount > stats.visibleCullableDrawItemCount"),
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
}
