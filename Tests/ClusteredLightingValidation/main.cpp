#include "Core/Log.h"
#include "Render/Lighting/ClusteredLighting.h"
#include "Render/Lighting/LightManager.h"
#include "RHI/RHI.h"
#include "RHI/RHICommandContext.h"

#include <gtest/gtest.h>

#include <cstring>
#include <span>
#include <string>
#include <vector>

using namespace RVX;

namespace
{
    class FakeBuffer final : public RHIBuffer
    {
    public:
        FakeBuffer(const RHIBufferDesc& desc, bool mapSucceeds = true)
            : m_desc(desc)
            , m_debugName(desc.debugName ? desc.debugName : "")
            , m_storage(static_cast<size_t>(desc.size))
            , m_mapSucceeds(mapSucceeds)
        {
        }

        uint64 GetSize() const override { return m_desc.size; }
        RHIBufferUsage GetUsage() const override { return m_desc.usage; }
        RHIMemoryType GetMemoryType() const override { return m_desc.memoryType; }
        uint32 GetStride() const override { return m_desc.stride; }

        void* Map() override
        {
            ++mapCount;
            if (!m_mapSucceeds || m_storage.empty())
                return nullptr;
            return m_storage.data();
        }

        void Unmap() override { ++unmapCount; }

        const std::string& GetDebugName() const { return m_debugName; }
        const std::vector<uint8>& GetStorage() const { return m_storage; }

        uint32 mapCount = 0;
        uint32 unmapCount = 0;

    private:
        RHIBufferDesc m_desc;
        std::string m_debugName;
        std::vector<uint8> m_storage;
        bool m_mapSucceeds = true;
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
    };

    class FakeDevice final : public IRHIDevice
    {
    public:
        RHIBufferRef CreateBuffer(const RHIBufferDesc& desc) override
        {
            ++createBufferCalls;
            createdBufferDescs.push_back(desc);
            createdBufferNames.emplace_back(desc.debugName ? desc.debugName : "");

            if (failBufferCreationCall > 0 && createBufferCalls == static_cast<uint32>(failBufferCreationCall))
                return {};

            const bool mapSucceeds = failMapBufferName.empty() || failMapBufferName != createdBufferNames.back();
            RHIBufferRef buffer(new FakeBuffer(desc, mapSucceeds));
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
        uint32 createBufferCalls = 0;
        int failBufferCreationCall = -1;
        std::string failMapBufferName;
        std::vector<RHIBufferDesc> createdBufferDescs;
        std::vector<std::string> createdBufferNames;
        std::vector<FakeBuffer*> createdBuffers;
    };

    ClusteringConfig SmallConfig()
    {
        ClusteringConfig config;
        config.clusterCountX = 2;
        config.clusterCountY = 2;
        config.clusterCountZ = 2;
        config.nearPlane = 0.1f;
        config.farPlane = 20.0f;
        config.maxLightsPerCluster = 4;
        return config;
    }

    Mat4 TestProjection()
    {
        return perspective(radians(60.0f), 1.0f, 0.1f, 20.0f);
    }

    Mat4 TestView()
    {
        return lookAt(Vec3(0.0f, 0.0f, 0.0f),
                      Vec3(0.0f, 0.0f, -1.0f),
                      Vec3(0.0f, 1.0f, 0.0f));
    }

    bool StorageHasAnyNonZeroByte(const std::vector<uint8>& storage)
    {
        for (uint8 value : storage)
        {
            if (value != 0)
                return true;
        }
        return false;
    }

} // namespace

class ClusteredLightingValidationFixture : public ::testing::Test
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

TEST_F(ClusteredLightingValidationFixture, NullDeviceAndInvalidConfigsAreRejected)
{
    ClusteredLighting lighting;
    EXPECT_FALSE(lighting.Initialize(nullptr, SmallConfig()));
    EXPECT_FALSE(lighting.IsInitialized());
    EXPECT_FALSE(lighting.GetLastError().empty());

    FakeDevice device;

    ClusteringConfig invalid = SmallConfig();
    invalid.clusterCountX = 0;
    EXPECT_FALSE(lighting.Initialize(&device, invalid));
    EXPECT_FALSE(lighting.IsInitialized());
    EXPECT_FALSE(lighting.GetLastError().empty());

    invalid = SmallConfig();
    invalid.nearPlane = 10.0f;
    invalid.farPlane = 1.0f;
    EXPECT_FALSE(lighting.Initialize(&device, invalid));
    EXPECT_FALSE(lighting.IsInitialized());
    EXPECT_FALSE(lighting.GetLastError().empty());

    invalid = SmallConfig();
    invalid.maxLightsPerCluster = 0;
    EXPECT_FALSE(lighting.Initialize(&device, invalid));
    EXPECT_FALSE(lighting.IsInitialized());
    EXPECT_FALSE(lighting.GetLastError().empty());
}

TEST_F(ClusteredLightingValidationFixture, ValidInitializationCreatesExpectedBuffers)
{
    FakeDevice device;
    ClusteredLighting lighting;

    ASSERT_TRUE(lighting.Initialize(&device, SmallConfig())) << lighting.GetLastError();
    EXPECT_TRUE(lighting.IsInitialized());
    EXPECT_TRUE(lighting.GetLastError().empty());

    ASSERT_EQ(device.createdBufferDescs.size(), static_cast<size_t>(4));
    EXPECT_EQ(device.createdBufferNames[0], "ClusterAABBBuffer");
    EXPECT_EQ(device.createdBufferNames[1], "ClusterDataBuffer");
    EXPECT_EQ(device.createdBufferNames[2], "ClusterLightIndexBuffer");
    EXPECT_EQ(device.createdBufferNames[3], "ClusterConstantsBuffer");

    const uint64 clusterCount = 8;
    EXPECT_EQ(device.createdBufferDescs[0].size, clusterCount * sizeof(Vec4) * 2u);
    EXPECT_EQ(device.createdBufferDescs[0].stride, sizeof(Vec4) * 2u);
    EXPECT_TRUE(HasFlag(device.createdBufferDescs[0].usage, RHIBufferUsage::Structured));
    EXPECT_TRUE(HasFlag(device.createdBufferDescs[0].usage, RHIBufferUsage::ShaderResource));

    EXPECT_EQ(device.createdBufferDescs[1].size, clusterCount * sizeof(GPUCluster));
    EXPECT_EQ(device.createdBufferDescs[1].stride, sizeof(GPUCluster));

    EXPECT_EQ(device.createdBufferDescs[2].size, clusterCount * SmallConfig().maxLightsPerCluster * sizeof(LightIndex));
    EXPECT_EQ(device.createdBufferDescs[2].stride, sizeof(LightIndex));

    EXPECT_EQ(device.createdBufferDescs[3].size, 256u);
    EXPECT_TRUE(HasFlag(device.createdBufferDescs[3].usage, RHIBufferUsage::Constant));
    EXPECT_EQ(sizeof(LightIndex), sizeof(uint32));
    EXPECT_EQ(sizeof(GPUClusterConstants), 96u);

    EXPECT_EQ(lighting.GetStatistics().clusterCount, 8u);
}

TEST_F(ClusteredLightingValidationFixture, BufferCreationFailureLeavesUninitialized)
{
    FakeDevice device;
    device.failBufferCreationCall = 3;

    ClusteredLighting lighting;
    EXPECT_FALSE(lighting.Initialize(&device, SmallConfig()));
    EXPECT_FALSE(lighting.IsInitialized());
    EXPECT_EQ(lighting.GetClusterAABBBuffer(), nullptr);
    EXPECT_EQ(lighting.GetClusterBuffer(), nullptr);
    EXPECT_FALSE(lighting.GetLastError().empty());
}

TEST_F(ClusteredLightingValidationFixture, ReconfigurePreservesDeviceAndRebuildsBuffers)
{
    FakeDevice device;
    ClusteredLighting lighting;
    ASSERT_TRUE(lighting.Initialize(&device, SmallConfig())) << lighting.GetLastError();

    RHIBuffer* oldClusterBuffer = lighting.GetClusterBuffer();
    const uint32 callsBeforeInvalidReconfigure = device.createBufferCalls;

    ClusteringConfig invalid = SmallConfig();
    invalid.clusterCountX = 0;
    EXPECT_FALSE(lighting.Reconfigure(invalid));
    EXPECT_TRUE(lighting.IsInitialized());
    EXPECT_EQ(lighting.GetClusterBuffer(), oldClusterBuffer);
    EXPECT_EQ(device.createBufferCalls, callsBeforeInvalidReconfigure);

    ClusteringConfig reconfigured = SmallConfig();
    reconfigured.clusterCountX = 3;
    reconfigured.maxLightsPerCluster = 2;

    ASSERT_TRUE(lighting.Reconfigure(reconfigured)) << lighting.GetLastError();
    EXPECT_TRUE(lighting.IsInitialized());
    EXPECT_EQ(device.createBufferCalls, 8u);
    EXPECT_EQ(lighting.GetStatistics().clusterCount, 12u);
    EXPECT_EQ(device.createdBufferDescs[4].size, 12u * sizeof(Vec4) * 2u);
    EXPECT_EQ(device.createdBufferDescs[5].size, 12u * sizeof(GPUCluster));
    EXPECT_EQ(device.createdBufferDescs[6].size, 12u * reconfigured.maxLightsPerCluster * sizeof(LightIndex));
    EXPECT_EQ(device.createdBufferDescs.back().size, 256u);
}

TEST_F(ClusteredLightingValidationFixture, BeginFrameAssignLightsAndUploadWritesBuffers)
{
    FakeDevice device;
    ClusteredLighting lighting;
    ASSERT_TRUE(lighting.Initialize(&device, SmallConfig())) << lighting.GetLastError();

    ASSERT_TRUE(lighting.BeginFrame(TestView(), TestProjection(), 640, 480)) << lighting.GetLastError();

    LightManager lights;
    lights.AddPointLight(Vec3(0.0f, 0.0f, -2.0f), Vec3(1.0f, 0.8f, 0.6f), 2.0f, 100.0f);
    lights.AddSpotLight(Vec3(0.0f, 1.0f, -3.0f),
                        Vec3(0.0f, -1.0f, 0.0f),
                        Vec3(0.4f, 0.6f, 1.0f),
                        3.0f,
                        100.0f,
                        radians(10.0f),
                        radians(25.0f));

    ASSERT_TRUE(lighting.AssignLights(lights)) << lighting.GetLastError();
    const auto stats = lighting.GetStatistics();
    EXPECT_EQ(stats.clusterCount, 8u);
    EXPECT_GT(stats.lightIndexCount, 0u);
    EXPECT_GT(stats.activeClusters, 0u);
    EXPECT_GT(stats.totalLightAssignments, 0u);

    ASSERT_TRUE(lighting.UploadFrameData()) << lighting.GetLastError();

    FakeBuffer* aabbBuffer = device.FindBuffer("ClusterAABBBuffer");
    FakeBuffer* clusterBuffer = device.FindBuffer("ClusterDataBuffer");
    FakeBuffer* lightIndexBuffer = device.FindBuffer("ClusterLightIndexBuffer");
    FakeBuffer* constantsBuffer = device.FindBuffer("ClusterConstantsBuffer");
    ASSERT_NE(aabbBuffer, nullptr);
    ASSERT_NE(clusterBuffer, nullptr);
    ASSERT_NE(lightIndexBuffer, nullptr);
    ASSERT_NE(constantsBuffer, nullptr);

    EXPECT_EQ(aabbBuffer->mapCount, 1u);
    EXPECT_EQ(clusterBuffer->mapCount, 1u);
    EXPECT_EQ(lightIndexBuffer->mapCount, 1u);
    EXPECT_EQ(constantsBuffer->mapCount, 1u);
    EXPECT_TRUE(StorageHasAnyNonZeroByte(clusterBuffer->GetStorage()));
    EXPECT_TRUE(StorageHasAnyNonZeroByte(lightIndexBuffer->GetStorage()));

    ASSERT_GE(constantsBuffer->GetStorage().size(), sizeof(GPUClusterConstants));
    GPUClusterConstants constants;
    std::memcpy(&constants, constantsBuffer->GetStorage().data(), sizeof(constants));
    EXPECT_EQ(constants.clusterSize.x, 2.0f);
    EXPECT_EQ(constants.clusterSize.y, 2.0f);
    EXPECT_EQ(constants.clusterSize.z, 2.0f);
    EXPECT_EQ(constants.clusterSize.w, 8.0f);
    EXPECT_EQ(constants.screenParams.x, 640.0f);
    EXPECT_EQ(constants.screenParams.y, 480.0f);
}

TEST_F(ClusteredLightingValidationFixture, EmptyLightAssignmentClearsFrameStats)
{
    FakeDevice device;
    ClusteredLighting lighting;
    ASSERT_TRUE(lighting.Initialize(&device, SmallConfig())) << lighting.GetLastError();
    ASSERT_TRUE(lighting.BeginFrame(TestView(), TestProjection(), 640, 480)) << lighting.GetLastError();

    LightManager lights;
    ASSERT_TRUE(lighting.AssignLights(lights)) << lighting.GetLastError();
    EXPECT_EQ(lighting.GetStatistics().clusterCount, 8u);
    EXPECT_EQ(lighting.GetStatistics().lightIndexCount, 0u);
    EXPECT_EQ(lighting.GetStatistics().activeClusters, 0u);
    EXPECT_EQ(lighting.GetStatistics().totalLightAssignments, 0u);

    lights.AddPointLight(Vec3(0.0f, 0.0f, -2.0f), Vec3(1.0f), 1.0f, 100.0f);
    ASSERT_TRUE(lighting.AssignLights(lights)) << lighting.GetLastError();
    EXPECT_GT(lighting.GetStatistics().lightIndexCount, 0u);

    LightManager emptyLights;
    ASSERT_TRUE(lighting.AssignLights(emptyLights)) << lighting.GetLastError();
    EXPECT_EQ(lighting.GetStatistics().lightIndexCount, 0u);
    EXPECT_EQ(lighting.GetStatistics().activeClusters, 0u);
    EXPECT_EQ(lighting.GetStatistics().totalLightAssignments, 0u);

    FakeCommandContext ctx;
    ASSERT_TRUE(lighting.UpdateGPUBuffers(ctx)) << lighting.GetLastError();
    FakeBuffer* lightIndexBuffer = device.FindBuffer("ClusterLightIndexBuffer");
    ASSERT_NE(lightIndexBuffer, nullptr);
    EXPECT_EQ(lightIndexBuffer->mapCount, 0u);
}

TEST_F(ClusteredLightingValidationFixture, OperationsRejectInvalidOrder)
{
    ClusteredLighting lighting;
    LightManager lights;
    FakeCommandContext ctx;

    EXPECT_FALSE(lighting.AssignLights(lights));
    EXPECT_FALSE(lighting.GetLastError().empty());
    EXPECT_FALSE(lighting.UploadFrameData());
    EXPECT_FALSE(lighting.GetLastError().empty());
    EXPECT_FALSE(lighting.UpdateGPUBuffers(ctx));
    EXPECT_FALSE(lighting.GetLastError().empty());

    FakeDevice device;
    ASSERT_TRUE(lighting.Initialize(&device, SmallConfig())) << lighting.GetLastError();
    EXPECT_FALSE(lighting.UploadFrameData());
    EXPECT_FALSE(lighting.GetLastError().empty());
    EXPECT_FALSE(lighting.UpdateGPUBuffers(ctx));
    EXPECT_FALSE(lighting.GetLastError().empty());
    EXPECT_FALSE(lighting.AssignLights(lights));
    EXPECT_FALSE(lighting.GetLastError().empty());
    EXPECT_FALSE(lighting.BeginFrame(TestView(), TestProjection(), 0, 480));
    EXPECT_FALSE(lighting.GetLastError().empty());
}

TEST_F(ClusteredLightingValidationFixture, PartialUploadFailureIsVisible)
{
    FakeDevice device;
    device.failMapBufferName = "ClusterDataBuffer";

    ClusteredLighting lighting;
    ASSERT_TRUE(lighting.Initialize(&device, SmallConfig())) << lighting.GetLastError();
    ASSERT_TRUE(lighting.BeginFrame(TestView(), TestProjection(), 640, 480)) << lighting.GetLastError();

    LightManager lights;
    ASSERT_TRUE(lighting.AssignLights(lights)) << lighting.GetLastError();

    EXPECT_FALSE(lighting.UploadFrameData());
    EXPECT_FALSE(lighting.GetLastError().empty());

    FakeBuffer* aabbBuffer = device.FindBuffer("ClusterAABBBuffer");
    FakeBuffer* clusterBuffer = device.FindBuffer("ClusterDataBuffer");
    FakeBuffer* constantsBuffer = device.FindBuffer("ClusterConstantsBuffer");
    ASSERT_NE(aabbBuffer, nullptr);
    ASSERT_NE(clusterBuffer, nullptr);
    ASSERT_NE(constantsBuffer, nullptr);

    EXPECT_EQ(aabbBuffer->mapCount, 1u);
    EXPECT_EQ(clusterBuffer->mapCount, 1u);
    EXPECT_EQ(clusterBuffer->unmapCount, 0u);
    EXPECT_EQ(constantsBuffer->mapCount, 0u);
}
