#include "Core/Log.h"
#include "Core/MathTypes.h"
#include "Render/Material/MaterialClassification.h"
#include "RHI/RHI.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <iterator>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#define private public
#include "Render/PipelineCache.h"
#undef private

#include "Render/GPUResourceManager.h"
#include "Render/Graph/ResourceViewCache.h"
#include "Render/Material/MaterialSystem.h"
#include "Render/Passes/DepthPrepass.h"
#include "Render/Passes/IRenderPass.h"
#include "Render/Passes/OpaquePass.h"
#include "Render/Passes/ShadowPass.h"
#include "Render/Passes/SkyboxPass.h"
#include "Render/Passes/TransparentPass.h"
#include "Render/PostProcess/Bloom.h"
#include "Render/PostProcess/ChromaticAberration.h"
#include "Render/PostProcess/ColorGrading.h"
#include "Render/PostProcess/FXAA.h"
#include "Render/PostProcess/PostProcessStack.h"
#include "Render/PostProcess/ToneMapping.h"
#include "Render/PostProcess/Vignette.h"
#include "Render/Renderer/RenderScene.h"
#include "Render/Renderer/ViewData.h"
#include "Renderer/RenderPassRegistry.h"
#include "Resource/Types/MaterialResource.h"
#include "Resource/Types/MeshResource.h"
#include "Resource/Types/TextureResource.h"
#include "RHI/RHICommandContext.h"
#include "RHI/RHIDevice.h"
#include "RHI/RHIUpload.h"
#include "Scene/Mesh.h"

#include <gtest/gtest.h>

using namespace RVX;

namespace
{
    namespace fs = std::filesystem;

    fs::path FindShaderDirectory()
    {
        fs::path cursor = fs::current_path();
        for (uint32 i = 0; i < 8; ++i)
        {
            fs::path candidate = cursor / "Render" / "Shaders";
            if (fs::exists(candidate / "DefaultLit.hlsl"))
            {
                return candidate;
            }

            if (!cursor.has_parent_path() || cursor == cursor.parent_path())
                break;

            cursor = cursor.parent_path();
        }

        return {};
    }

    std::string ReadTextFile(const fs::path& path)
    {
        std::ifstream stream(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
    }

    class FakeBuffer final : public RHIBuffer
    {
    public:
        explicit FakeBuffer(const RHIBufferDesc& desc, bool mapSucceeds = true)
            : m_desc(desc)
            , m_storage(static_cast<size_t>(desc.size))
            , m_mapSucceeds(mapSucceeds)
        {
        }

        uint64 GetSize() const override { return m_desc.size; }
        RHIBufferUsage GetUsage() const override { return m_desc.usage; }
        RHIMemoryType GetMemoryType() const override { return m_desc.memoryType; }
        uint32 GetStride() const override { return m_desc.stride; }

        void* Map() override { return !m_mapSucceeds || m_storage.empty() ? nullptr : m_storage.data(); }
        void Unmap() override {}

        const std::vector<uint8>& GetStorage() const { return m_storage; }

    private:
        RHIBufferDesc m_desc;
        std::vector<uint8> m_storage;
        bool m_mapSucceeds = true;
    };

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
            , m_format(desc.format == RHIFormat::Unknown && texture ? texture->GetFormat() : desc.format)
            , m_range(desc.subresourceRange)
        {
        }

        RHITexture* GetTexture() const override { return m_texture; }
        RHIFormat GetFormat() const override { return m_format; }
        const RHISubresourceRange& GetSubresourceRange() const override { return m_range; }

    private:
        RHITexture* m_texture = nullptr;
        RHIFormat m_format = RHIFormat::Unknown;
        RHISubresourceRange m_range;
    };

    class FakeSampler final : public RHISampler
    {
    };

    class FakeShader final : public RHIShader
    {
    public:
        explicit FakeShader(const RHIShaderDesc& desc)
            : m_stage(desc.stage)
        {
            if (desc.bytecode && desc.bytecodeSize > 0)
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

        const std::vector<RHIBindingLayoutEntry>& GetEntries() const override { return m_entries; }

    private:
        std::vector<RHIBindingLayoutEntry> m_entries;
    };

    class FakePipelineLayout final : public RHIPipelineLayout
    {
    };

    class FakePipeline final : public RHIPipeline
    {
    public:
        bool IsCompute() const override { return false; }
    };

    class FakeDescriptorSet final : public RHIDescriptorSet
    {
    public:
        explicit FakeDescriptorSet(const RHIDescriptorSetDesc& desc)
            : bindings(desc.bindings)
        {
        }

        bool Update(const std::vector<RHIDescriptorBinding>& newBindings) override
        {
            bindings = newBindings;
            return true;
        }

        std::vector<RHIDescriptorBinding> bindings;
    };

    class FakeStagingBuffer final : public RHIStagingBuffer
    {
    public:
        explicit FakeStagingBuffer(const RHIStagingBufferDesc& desc)
            : m_desc(desc)
        {
            RHIBufferDesc bufferDesc;
            bufferDesc.size = desc.size;
            bufferDesc.usage = RHIBufferUsage::CopySrc;
            bufferDesc.memoryType = RHIMemoryType::Upload;
            bufferDesc.debugName = desc.debugName;
            m_buffer = RHIBufferRef(new FakeBuffer(bufferDesc));
        }

        void* Map(uint64 offset = 0, uint64 size = RVX_WHOLE_SIZE) override
        {
            (void)size;
            auto* fakeBuffer = static_cast<FakeBuffer*>(m_buffer.Get());
            auto* storage = fakeBuffer->GetStorage().data();
            return const_cast<uint8*>(storage + offset);
        }

        void Unmap() override {}
        uint64 GetSize() const override { return m_desc.size; }
        RHIBuffer* GetBuffer() const override { return m_buffer.Get(); }

    private:
        RHIStagingBufferDesc m_desc;
        RHIBufferRef m_buffer;
    };

    class RecordingCommandContext final : public RHICommandContext
    {
    public:
        void Begin() override { ++beginCount; }
        void End() override { ++endCount; }
        void Reset() override {}
        void BeginEvent(const char*, uint32 = 0) override {}
        void EndEvent() override {}
        void SetMarker(const char*, uint32 = 0) override {}
        void BufferBarrier(const RHIBufferBarrier&) override { ++bufferBarrierCount; }
        void TextureBarrier(const RHITextureBarrier&) override { ++textureBarrierCount; }
        void Barriers(std::span<const RHIBufferBarrier>, std::span<const RHITextureBarrier>) override {}
        void BeginBarrier(const RHIBufferBarrier&) override {}
        void BeginBarrier(const RHITextureBarrier&) override {}
        void EndBarrier(const RHIBufferBarrier&) override {}
        void EndBarrier(const RHITextureBarrier&) override {}
        void BeginRenderPass(const RHIRenderPassDesc& desc) override
        {
            ++beginRenderPassCount;
            renderPasses.push_back(desc);
            callSequence.push_back("BeginRenderPass");
        }
        void EndRenderPass() override
        {
            ++endRenderPassCount;
            callSequence.push_back("EndRenderPass");
        }
        void SetPipeline(RHIPipeline* pipeline) override
        {
            pipelineSequence.push_back(pipeline);
            callSequence.push_back("SetPipeline");
        }
        void SetVertexBuffer(uint32, RHIBuffer*, uint64 = 0) override {}
        void SetVertexBuffers(uint32, std::span<RHIBuffer* const>, std::span<const uint64> = {}) override {}
        void SetIndexBuffer(RHIBuffer*, RHIFormat, uint64 = 0) override {}
        void SetDescriptorSet(uint32 set, RHIDescriptorSet*, std::span<const uint32> = {}) override
        {
            descriptorSetSequence.push_back(set);
            callSequence.push_back("SetDescriptorSet");
        }
        void SetPushConstants(const void*, uint32, uint32 = 0) override {}
        void SetViewport(const RHIViewport& viewport) override
        {
            viewports.push_back(viewport);
            callSequence.push_back("SetViewport");
        }
        void SetViewports(std::span<const RHIViewport>) override {}
        void SetScissor(const RHIRect& scissor) override
        {
            scissors.push_back(scissor);
            callSequence.push_back("SetScissor");
        }
        void SetScissors(std::span<const RHIRect>) override {}
        void Draw(uint32 vertexCount, uint32 = 1, uint32 = 0, uint32 = 0) override
        {
            ++drawCount;
            lastDrawVertexCount = vertexCount;
            callSequence.push_back("Draw");
        }
        void DrawIndexed(uint32, uint32 = 1, uint32 = 0, int32 = 0, uint32 = 0) override
        {
            ++drawIndexedCount;
        }
        void DrawIndirect(RHIBuffer*, uint64, uint32, uint32) override {}
        void DrawIndexedIndirect(RHIBuffer*, uint64, uint32, uint32) override {}
        void Dispatch(uint32, uint32, uint32) override {}
        void DispatchIndirect(RHIBuffer*, uint64) override {}
        void CopyBuffer(RHIBuffer*, RHIBuffer*, uint64, uint64, uint64) override { ++copyBufferCount; }
        void CopyTexture(RHITexture*, RHITexture*, const RHITextureCopyDesc& = {}) override {}
        void CopyBufferToTexture(RHIBuffer*, RHITexture*, const RHIBufferTextureCopyDesc&) override
        {
            ++copyBufferToTextureCount;
        }
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

        uint32 beginCount = 0;
        uint32 endCount = 0;
        uint32 beginRenderPassCount = 0;
        uint32 endRenderPassCount = 0;
        uint32 bufferBarrierCount = 0;
        uint32 textureBarrierCount = 0;
        uint32 copyBufferCount = 0;
        uint32 copyBufferToTextureCount = 0;
        uint32 drawCount = 0;
        uint32 drawIndexedCount = 0;
        uint32 lastDrawVertexCount = 0;
        std::vector<RHIRenderPassDesc> renderPasses;
        std::vector<RHIPipeline*> pipelineSequence;
        std::vector<uint32> descriptorSetSequence;
        std::vector<RHIViewport> viewports;
        std::vector<RHIRect> scissors;
        std::vector<std::string> callSequence;
    };

    class FakeFence final : public RHIFence
    {
    public:
        explicit FakeFence(uint64 initialValue)
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
        RHIBufferRef CreateBuffer(const RHIBufferDesc& desc) override
        {
            createdBufferDescs.push_back(desc);
            auto buffer = RHIBufferRef(new FakeBuffer(desc, bufferMapSucceeds));
            createdBuffers.push_back(static_cast<FakeBuffer*>(buffer.Get()));
            return buffer;
        }

        RHITextureRef CreateTexture(const RHITextureDesc& desc) override
        {
            createdTextureDescs.push_back(desc);
            return RHITextureRef(new FakeTexture(desc));
        }

        RHITextureViewRef CreateTextureView(RHITexture* texture, const RHITextureViewDesc& desc = {}) override
        {
            createdTextureViewDescs.push_back(desc);
            const bool isDirectionalShadowSRV = desc.debugName && std::string(desc.debugName) == "DirectionalShadowSRV" &&
                                                desc.type == RHITextureViewType::ShaderResource;
            if (failDirectionalShadowSRVCreation && isDirectionalShadowSRV)
                return {};
            if (!textureViewCreationSucceeds)
                return {};
            return RHITextureViewRef(new FakeTextureView(texture, desc));
        }

        RHISamplerRef CreateSampler(const RHISamplerDesc&) override
        {
            if (!samplerCreationSucceeds)
                return {};
            return RHISamplerRef(new FakeSampler());
        }

        RHIShaderRef CreateShader(const RHIShaderDesc& desc) override
        {
            return RHIShaderRef(new FakeShader(desc));
        }

        RHIHeapRef CreateHeap(const RHIHeapDesc&) override { return nullptr; }
        RHITextureRef CreatePlacedTexture(RHIHeap*, uint64, const RHITextureDesc&) override { return nullptr; }
        RHIBufferRef CreatePlacedBuffer(RHIHeap*, uint64, const RHIBufferDesc&) override { return nullptr; }
        MemoryRequirements GetTextureMemoryRequirements(const RHITextureDesc&) override { return {}; }
        MemoryRequirements GetBufferMemoryRequirements(const RHIBufferDesc&) override { return {}; }

        RHIDescriptorSetLayoutRef CreateDescriptorSetLayout(const RHIDescriptorSetLayoutDesc& desc) override
        {
            return RHIDescriptorSetLayoutRef(new FakeDescriptorSetLayout(desc));
        }

        RHIPipelineLayoutRef CreatePipelineLayout(const RHIPipelineLayoutDesc&) override
        {
            return RHIPipelineLayoutRef(new FakePipelineLayout());
        }

        RHIPipelineRef CreateGraphicsPipeline(const RHIGraphicsPipelineDesc&) override
        {
            return RHIPipelineRef(new FakePipeline());
        }

        RHIPipelineRef CreateComputePipeline(const RHIComputePipelineDesc&) override { return nullptr; }

        RHIDescriptorSetRef CreateDescriptorSet(const RHIDescriptorSetDesc& desc) override
        {
            createdDescriptorSetDescs.push_back(desc);
            return RHIDescriptorSetRef(new FakeDescriptorSet(desc));
        }

        RHIQueryPoolRef CreateQueryPool(const RHIQueryPoolDesc&) override { return nullptr; }

        RHICommandContextRef CreateCommandContext(RHICommandQueueType) override
        {
            return RHICommandContextRef(new RecordingCommandContext());
        }

        uint64 SubmitCommandContext(RHICommandContext*, RHIFence* signalFence = nullptr) override
        {
            if (!signalFence)
                return 0;

            const uint64 fenceValue = m_nextFenceValue++;
            signalFence->Signal(fenceValue);
            return fenceValue;
        }

        uint64 SubmitCommandContexts(std::span<RHICommandContext* const>, RHIFence* signalFence = nullptr) override
        {
            return SubmitCommandContext(nullptr, signalFence);
        }

        RHISwapChainRef CreateSwapChain(const RHISwapChainDesc&) override { return nullptr; }

        RHIFenceRef CreateFence(uint64 initialValue = 0) override
        {
            auto fence = RHIFenceRef(new FakeFence(initialValue));
            m_fences.push_back(fence);
            return fence;
        }

        void WaitForFence(RHIFence* fence, uint64 value) override
        {
            if (fence)
                fence->Wait(value);
        }

        void WaitIdle() override
        {
            for (const RHIFenceRef& fence : m_fences)
            {
                if (fence)
                    fence->Signal(UINT64_MAX);
            }
        }

        void BeginFrame() override {}
        void EndFrame() override {}
        uint32 GetCurrentFrameIndex() const override { return 0; }

        RHIStagingBufferRef CreateStagingBuffer(const RHIStagingBufferDesc& desc) override
        {
            return RHIStagingBufferRef(new FakeStagingBuffer(desc));
        }

        RHIRingBufferRef CreateRingBuffer(const RHIRingBufferDesc&) override { return nullptr; }
        RHIMemoryStats GetMemoryStats() const override { return {}; }
        void BeginResourceGroup(const char*) override {}
        void EndResourceGroup() override {}
        const RHICapabilities& GetCapabilities() const override { return m_capabilities; }
        RHIBackendType GetBackendType() const override { return RHIBackendType::DX12; }

        bool bufferMapSucceeds = true;
        bool textureViewCreationSucceeds = true;
        bool failDirectionalShadowSRVCreation = false;
        bool samplerCreationSucceeds = true;
        std::vector<RHIBufferDesc> createdBufferDescs;
        std::vector<FakeBuffer*> createdBuffers;
        std::vector<RHIDescriptorSetDesc> createdDescriptorSetDescs;
        std::vector<RHITextureDesc> createdTextureDescs;
        std::vector<RHITextureViewDesc> createdTextureViewDescs;

    private:
        uint64 m_nextFenceValue = 1;
        std::vector<RHIFenceRef> m_fences;
        RHICapabilities m_capabilities;
    };

    class StatusTestPass final : public IRenderPass
    {
    public:
        StatusTestPass(std::string name,
                       int32_t priority,
                       bool requested,
                       bool supported,
                       std::string unsupportedReason = {})
            : m_name(std::move(name))
            , m_priority(priority)
            , m_requested(requested)
            , m_supported(supported)
            , m_unsupportedReason(std::move(unsupportedReason))
        {
        }

        const char* GetName() const override { return m_name.c_str(); }
        int32_t GetPriority() const override { return m_priority; }
        bool IsRequestedEnabled() const override { return m_requested; }
        bool IsSupported() const override { return m_supported; }
        const std::string& GetUnsupportedReason() const override { return m_unsupportedReason; }
        void Setup(RenderGraphBuilder&, const ViewData&) override {}
        void Execute(RHICommandContext&, const ViewData&) override {}

    private:
        std::string m_name;
        int32_t m_priority = 0;
        bool m_requested = false;
        bool m_supported = false;
        std::string m_unsupportedReason;
    };

    class RecordingPostProcessPass final : public IPostProcessPass
    {
    public:
        RecordingPostProcessPass(std::string name, int32 priority)
            : m_name(std::move(name))
            , m_priority(priority)
        {
            m_enabled = true;
            m_supported = true;
        }

        const char* GetName() const override { return m_name.c_str(); }
        int32 GetPriority() const override { return m_priority; }
        void Configure(const PostProcessSettings&) override {}

        void AddToGraph(RenderGraph& graph, RGTextureHandle input, RGTextureHandle output) override
        {
            lastInput = input;
            lastOutput = output;
            addToGraphCount++;

            struct PassData
            {
                RGTextureHandle input;
                RGTextureHandle output;
            };

            graph.AddPass<PassData>(
                m_name.c_str(),
                RenderGraphPassType::Graphics,
                [input, output](RenderGraphBuilder& builder, PassData& data)
                {
                    data.input = builder.Read(input);
                    data.output = builder.Write(output, RHIResourceState::RenderTarget);
                },
                [](const PassData&, RHICommandContext&) {});
        }

        RGTextureHandle lastInput;
        RGTextureHandle lastOutput;
        uint32 addToGraphCount = 0;

    private:
        std::string m_name;
        int32 m_priority = 0;
    };

    std::unique_ptr<Resource::MeshResource> CreateMeshResource(Resource::ResourceId id)
    {
        auto resource = std::make_unique<Resource::MeshResource>();
        resource->SetId(id);
        resource->SetName("RenderPassMesh");
        resource->SetMesh(MeshFactory::CreateTriangle());
        return resource;
    }

    RenderObject MakeRenderObject(Resource::MeshResource& meshResource)
    {
        RenderObject object;
        object.meshId = meshResource.GetId();
        object.meshResource = &meshResource;
        object.worldMatrix = Mat4Identity();
        object.normalMatrix = Mat4Identity();
        object.visible = true;
        return object;
    }

    RenderDrawItem MakeDrawItem(MaterialRenderMode mode)
    {
        RenderDrawItem item;
        item.objectIndex = 0;
        item.submeshIndex = 0;
        item.meshId = 401;
        item.materialId = static_cast<uint64>(mode) + 1;
        item.renderMode = mode;
        return item;
    }

    Resource::TextureHandle CreateTextureResource(Resource::ResourceId id)
    {
        auto* texture = new Resource::TextureResource();
        texture->SetId(id);
        texture->SetName("RenderPassTexture");

        Resource::TextureMetadata metadata;
        metadata.width = 1;
        metadata.height = 1;
        metadata.format = Resource::TextureFormat::RGBA8;
        metadata.isSRGB = false;

        texture->SetData({255, 255, 255, 255}, metadata);
        return Resource::TextureHandle(texture);
    }

    void ConfigureMaterialWithAlbedo(Resource::MaterialResource& materialResource,
                                     const Resource::TextureHandle& albedo)
    {
        materialResource.SetId(501);
        materialResource.SetName("RenderPassFallbackMaterial");
        materialResource.SetMaterialData(std::make_shared<Material>());
        materialResource.SetTexture("albedo", albedo);
    }

    bool IsIdentityMatrix(const Mat4& matrix, float epsilon = 0.0001f)
    {
        const Mat4 identity = Mat4Identity();
        for (int column = 0; column < 4; ++column)
        {
            for (int row = 0; row < 4; ++row)
            {
                if (std::abs(matrix[column][row] - identity[column][row]) > epsilon)
                    return false;
            }
        }
        return true;
    }

    const FakeBuffer* FindCreatedBuffer(const FakeDevice& device, const char* debugName)
    {
        for (size_t i = 0; i < device.createdBufferDescs.size() && i < device.createdBuffers.size(); ++i)
        {
            const char* name = device.createdBufferDescs[i].debugName;
            if (name && std::string(name) == debugName)
            {
                return device.createdBuffers[i];
            }
        }

        return nullptr;
    }

    class RenderPassValidationFixture : public ::testing::Test
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

        void SetUp() override {}

        void Initialize(bool bufferMapSucceeds = true)
        {
            const fs::path shaderDir = FindShaderDirectory();
            if (shaderDir.empty())
            {
                GTEST_SKIP() << "Render/Shaders directory not found";
            }

            device.bufferMapSucceeds = bufferMapSucceeds;

            ASSERT_TRUE(pipelineCache.Initialize(&device, shaderDir.string())) << pipelineCache.GetLastError();

            gpuResources.Initialize(&device);
            viewCache.Initialize(&device);
            ASSERT_TRUE(materialSystem.Initialize(&device, &gpuResources, pipelineCache.GetMaterialSetLayout()));

            meshResource = CreateMeshResource(401);
            gpuResources.UploadImmediate(meshResource.get());
            ASSERT_TRUE(gpuResources.IsGPUReady(meshResource->GetId()));

            scene.AddObject(MakeRenderObject(*meshResource));

            colorTexture = device.CreateTexture(RHITextureDesc::Texture2D(64, 64, RHIFormat::RGBA8_UNORM));
            ASSERT_TRUE(colorTexture);
            colorView = device.CreateTextureView(colorTexture.Get());
            ASSERT_TRUE(colorView);
        }

        void TearDown() override
        {
            materialSystem.Shutdown();
            viewCache.Shutdown();
            gpuResources.Shutdown();
            pipelineCache.Shutdown();
        }

        FakeDevice device;
        PipelineCache pipelineCache;
        GPUResourceManager gpuResources;
        ResourceViewCache viewCache;
        MaterialSystem materialSystem;
        RenderScene scene;
        std::unique_ptr<Resource::MeshResource> meshResource;
        RHITextureRef colorTexture;
        RHITextureViewRef colorView;
        ViewData view;
    };
} // namespace

TEST(RenderPassStatusValidation, DefaultImplementedPassReportsEnabledAndSupported)
{
    StatusTestPass pass("Implemented", 20, true, true);

    RenderPassStatus status = pass.GetStatus();

    EXPECT_EQ("Implemented", status.name);
    EXPECT_EQ(20, status.priority);
    EXPECT_TRUE(status.requestedEnabled);
    EXPECT_TRUE(status.supported);
    EXPECT_TRUE(status.enabled);
    EXPECT_TRUE(status.unsupportedReason.empty());
}

TEST(RenderPassStatusValidation, RequestedUnsupportedPassIsNotEnabled)
{
    StatusTestPass pass("Unsupported", 30, true, false, "No pipeline");

    RenderPassStatus status = pass.GetStatus();

    EXPECT_TRUE(status.requestedEnabled);
    EXPECT_FALSE(status.supported);
    EXPECT_FALSE(status.enabled);
    EXPECT_EQ("No pipeline", status.unsupportedReason);
}

TEST(RenderPassStatusValidation, RegistryStatusSnapshotsPreserveSortedPassOrder)
{
    RenderPassRegistry registry;

    registry.AddPass(std::make_unique<StatusTestPass>("Late", 300, true, true), nullptr);
    registry.AddPass(std::make_unique<StatusTestPass>("Early", 100, false, true), nullptr);
    registry.AddPass(std::make_unique<StatusTestPass>("MiddleUnsupported", 200, true, false, "No target"), nullptr);

    std::vector<RenderPassStatus> statuses = registry.GetPassStatuses();

    ASSERT_EQ(static_cast<size_t>(3), statuses.size());
    EXPECT_EQ("Early", statuses[0].name);
    EXPECT_FALSE(statuses[0].requestedEnabled);
    EXPECT_FALSE(statuses[0].enabled);

    EXPECT_EQ("MiddleUnsupported", statuses[1].name);
    EXPECT_TRUE(statuses[1].requestedEnabled);
    EXPECT_FALSE(statuses[1].supported);
    EXPECT_FALSE(statuses[1].enabled);
    EXPECT_EQ("No target", statuses[1].unsupportedReason);

    EXPECT_EQ("Late", statuses[2].name);
    EXPECT_TRUE(statuses[2].enabled);
}

TEST(RenderPassStatusValidation, BuiltInProductionPassStatusesAreHonest)
{
    DepthPrepass depthPrepass;
    EXPECT_FALSE(depthPrepass.IsRequestedEnabled());
    EXPECT_FALSE(depthPrepass.IsEnabled());

    depthPrepass.SetEnabled(true);
    EXPECT_TRUE(depthPrepass.IsRequestedEnabled());
    EXPECT_FALSE(depthPrepass.IsSupported());
    EXPECT_FALSE(depthPrepass.IsEnabled());
    EXPECT_FALSE(depthPrepass.GetUnsupportedReason().empty());

    ShadowPass shadowPass;
    EXPECT_FALSE(shadowPass.IsRequestedEnabled());
    EXPECT_FALSE(shadowPass.IsEnabled());

    shadowPass.SetDirectionalLight(Vec3{0.0f, -1.0f, 0.0f}, Vec3{1.0f, 1.0f, 1.0f}, 1.0f);
    EXPECT_TRUE(shadowPass.IsRequestedEnabled());
    EXPECT_FALSE(shadowPass.IsSupported());
    EXPECT_FALSE(shadowPass.IsEnabled());
    EXPECT_FALSE(shadowPass.GetUnsupportedReason().empty());

    SkyboxPass skyboxPass;
    EXPECT_TRUE(skyboxPass.IsRequestedEnabled());
    EXPECT_FALSE(skyboxPass.IsSupported());
    EXPECT_FALSE(skyboxPass.IsEnabled());
    EXPECT_FALSE(skyboxPass.GetUnsupportedReason().empty());

    OpaquePass opaquePass;
    EXPECT_TRUE(opaquePass.IsRequestedEnabled());
    EXPECT_TRUE(opaquePass.IsSupported());
    EXPECT_TRUE(opaquePass.IsEnabled());

    TransparentPass transparentPass;
    EXPECT_TRUE(transparentPass.IsRequestedEnabled());
    EXPECT_TRUE(transparentPass.IsSupported());
    EXPECT_TRUE(transparentPass.IsEnabled());
}

TEST_F(RenderPassValidationFixture, ShadowPassReportsSupportedWithDepthPipelineAndResources)
{
    ASSERT_NO_FATAL_FAILURE(Initialize());

    ShadowPass pass;
    pass.SetResources(&gpuResources, &pipelineCache);
    pass.SetDirectionalLight(Vec3{0.0f, -1.0f, 0.0f}, Vec3{1.0f, 1.0f, 1.0f}, 1.0f);

    EXPECT_TRUE(pass.IsRequestedEnabled());
    EXPECT_TRUE(pass.IsSupported()) << pass.GetUnsupportedReason();
    EXPECT_TRUE(pass.IsEnabled());

    pipelineCache.m_depthOnlyPipeline.Reset();
    EXPECT_FALSE(pass.IsSupported());
    EXPECT_FALSE(pass.GetUnsupportedReason().empty());
}

TEST_F(RenderPassValidationFixture, ShadowPassRejectsUnsupportedCascadeCounts)
{
    ASSERT_NO_FATAL_FAILURE(Initialize());

    ShadowPassConfig config;
    config.numCascades = RVX_MAX_DIRECTIONAL_SHADOW_CASCADES + 1;

    ShadowPass pass;
    pass.SetResources(&gpuResources, &pipelineCache);
    pass.SetConfig(config);
    pass.SetDirectionalLight(Vec3{0.0f, -1.0f, 0.0f}, Vec3{1.0f, 1.0f, 1.0f}, 1.0f);

    EXPECT_FALSE(pass.IsSupported());
    EXPECT_NE(pass.GetUnsupportedReason().find("cascade"), std::string::npos);
}

TEST_F(RenderPassValidationFixture, ShadowPassDisabledDoesNotDeclareOrDrawCascadeResources)
{
    ASSERT_NO_FATAL_FAILURE(Initialize());

    RenderGraph graph;
    graph.SetDevice(&device);

    view.renderGraph = &graph;
    view.viewCache = &viewCache;

    ShadowPass pass;
    pass.SetResources(&gpuResources, &pipelineCache);
    pass.SetRenderScene(&scene);

    EXPECT_FALSE(pass.IsRequestedEnabled());
    EXPECT_FALSE(pass.IsEnabled());

    pass.AddToGraph(graph, view);

    EXPECT_TRUE(pass.GetCascadeTextureHandles().empty());
    EXPECT_EQ(pass.GetStats().declaredCascadeResourceCount, 0u);

    graph.Compile();
    const auto& stats = graph.GetCompileStats();
    EXPECT_TRUE(stats.compileValid);
    EXPECT_EQ(stats.totalPasses, 1u);
    EXPECT_EQ(stats.emptyPassUsageCount, 1u);

    RecordingCommandContext ctx;
    graph.Execute(ctx);

    EXPECT_EQ(ctx.beginRenderPassCount, 0u);
    EXPECT_EQ(ctx.drawIndexedCount, 0u);
    EXPECT_EQ(pass.GetStats().resolvedCascadeViewCount, 0u);
    EXPECT_EQ(pass.GetStats().drawCount, 0u);
}

TEST_F(RenderPassValidationFixture, ShadowPassSetupDeclaresCascadeDepthResourcesAndPSSMMatrices)
{
    ASSERT_NO_FATAL_FAILURE(Initialize());

    RenderGraph graph;
    graph.SetDevice(&device);

    view.renderGraph = &graph;
    view.viewCache = &viewCache;
    view.viewportWidth = 320;
    view.viewportHeight = 180;
    view.aspectRatio = 16.0f / 9.0f;
    view.fieldOfView = 1.0472f;
    view.nearPlane = 0.1f;
    view.farPlane = 100.0f;
    view.cameraPosition = Vec3(0.0f, 0.0f, 5.0f);
    view.cameraForward = Vec3(0.0f, 0.0f, -1.0f);
    view.inverseViewMatrix = Mat4Identity();

    ShadowPassConfig config;
    config.numCascades = 3;
    config.shadowMapSize = 128;

    ShadowPass pass;
    pass.SetResources(&gpuResources, &pipelineCache);
    pass.SetRenderScene(&scene);
    pass.SetConfig(config);
    pass.SetDirectionalLight(Vec3{-0.4f, -1.0f, -0.2f}, Vec3{1.0f, 1.0f, 1.0f}, 2.0f);

    pass.AddToGraph(graph, view);

    const auto& handles = pass.GetCascadeTextureHandles();
    ASSERT_EQ(handles.size(), static_cast<size_t>(3));
    ASSERT_TRUE(pass.GetShadowMapTextureHandle().IsValid());
    EXPECT_EQ(pass.GetStats().configuredCascadeCount, 3u);
    EXPECT_EQ(pass.GetStats().declaredCascadeResourceCount, 3u);

    for (uint32 i = 0; i < static_cast<uint32>(handles.size()); ++i)
    {
        const RGTextureHandle& handle = handles[i];
        EXPECT_EQ(handle.index, pass.GetShadowMapTextureHandle().index);
        EXPECT_TRUE(handle.hasSubresourceRange);
        EXPECT_EQ(handle.subresourceRange.baseArrayLayer, i);
        EXPECT_EQ(handle.subresourceRange.arrayLayerCount, 1u);
        EXPECT_EQ(handle.subresourceRange.aspect, RHITextureAspect::Depth);
        const RHITextureDesc* desc = graph.GetTextureDesc(handle);
        ASSERT_NE(desc, nullptr);
        EXPECT_EQ(desc->width, 128u);
        EXPECT_EQ(desc->height, 128u);
        EXPECT_EQ(desc->arraySize, 3u);
        EXPECT_EQ(desc->format, PipelineCache::GetDefaultDepthStencilFormat());
        EXPECT_TRUE(HasFlag(desc->usage, RHITextureUsage::DepthStencil));
    }

    const auto& cascades = pass.GetCascades();
    ASSERT_EQ(cascades.size(), static_cast<size_t>(3));
    float previousSplit = 0.0f;
    for (const ShadowCascade& cascade : cascades)
    {
        EXPECT_GT(cascade.splitDepth, previousSplit);
        EXPECT_LE(cascade.splitDepth, 1.0f);
        EXPECT_FALSE(IsIdentityMatrix(cascade.viewProjection));
        previousSplit = cascade.splitDepth;
    }

    graph.Compile();
    const auto& stats = graph.GetCompileStats();
    EXPECT_TRUE(stats.compileValid);
    EXPECT_EQ(stats.totalPasses, 1u);
    EXPECT_EQ(stats.culledPasses, 0u);
    EXPECT_EQ(stats.emptyPassUsageCount, 0u);
}

TEST_F(RenderPassValidationFixture, ShadowPassSingleCascadeStillDeclaresArrayCompatibleTexture)
{
    ASSERT_NO_FATAL_FAILURE(Initialize());

    RenderGraph graph;
    graph.SetDevice(&device);

    view.renderGraph = &graph;
    view.viewCache = &viewCache;
    view.viewportWidth = 64;
    view.viewportHeight = 64;
    view.aspectRatio = 1.0f;
    view.fieldOfView = 1.0472f;
    view.nearPlane = 0.1f;
    view.farPlane = 100.0f;
    view.cameraPosition = Vec3(0.0f, 0.0f, 5.0f);
    view.cameraForward = Vec3(0.0f, 0.0f, -1.0f);
    view.inverseViewMatrix = Mat4Identity();

    ShadowPassConfig config;
    config.numCascades = 1;
    config.shadowMapSize = 64;

    ShadowPass pass;
    pass.SetResources(&gpuResources, &pipelineCache);
    pass.SetRenderScene(&scene);
    pass.SetConfig(config);
    pass.SetDirectionalLight(Vec3{-0.4f, -1.0f, -0.2f}, Vec3{1.0f, 1.0f, 1.0f}, 2.0f);

    pass.AddToGraph(graph, view);

    ASSERT_TRUE(pass.GetShadowMapTextureHandle().IsValid());
    const RHITextureDesc* desc = graph.GetTextureDesc(pass.GetShadowMapTextureHandle());
    ASSERT_NE(desc, nullptr);
    EXPECT_EQ(desc->arraySize, RVX_MIN_DIRECTIONAL_SHADOW_ARRAY_LAYERS);
    ASSERT_EQ(pass.GetCascadeTextureHandles().size(), static_cast<size_t>(1));
    EXPECT_EQ(pass.GetCascadeTextureHandles()[0].subresourceRange.baseArrayLayer, 0u);
    EXPECT_EQ(pass.GetCascadeTextureHandles()[0].subresourceRange.aspect, RHITextureAspect::Depth);
}

TEST_F(RenderPassValidationFixture, ShadowPassExecuteResolvesCascadeViewsAndDrawsOnlyShadowCasters)
{
    ASSERT_NO_FATAL_FAILURE(Initialize());

    RenderObject nonCaster = MakeRenderObject(*meshResource);
    nonCaster.castsShadow = false;
    scene.AddObject(nonCaster);

    RenderGraph graph;
    graph.SetDevice(&device);

    view.renderGraph = &graph;
    view.viewCache = &viewCache;
    view.viewportWidth = 320;
    view.viewportHeight = 180;
    view.aspectRatio = 16.0f / 9.0f;
    view.fieldOfView = 1.0472f;
    view.nearPlane = 0.1f;
    view.farPlane = 100.0f;
    view.cameraPosition = Vec3(0.0f, 0.0f, 5.0f);
    view.cameraForward = Vec3(0.0f, 0.0f, -1.0f);
    view.inverseViewMatrix = Mat4Identity();

    ShadowPassConfig config;
    config.numCascades = 2;
    config.shadowMapSize = 64;

    ShadowPass pass;
    pass.SetResources(&gpuResources, &pipelineCache);
    pass.SetRenderScene(&scene);
    pass.SetConfig(config);
    pass.SetDirectionalLight(Vec3{-0.3f, -1.0f, -0.2f}, Vec3{1.0f, 1.0f, 1.0f}, 1.0f);

    pass.AddToGraph(graph, view);
    graph.Compile();

    RecordingCommandContext ctx;
    graph.Execute(ctx);

    EXPECT_EQ(ctx.beginRenderPassCount, 2u);
    EXPECT_EQ(ctx.endRenderPassCount, 2u);
    EXPECT_EQ(ctx.drawIndexedCount, 2u);
    EXPECT_EQ(pass.GetStats().resolvedCascadeViewCount, 2u);
    EXPECT_EQ(pass.GetStats().shadowCasterCount, 2u);
    EXPECT_EQ(pass.GetStats().drawCount, 2u);
    ASSERT_EQ(ctx.renderPasses.size(), static_cast<size_t>(2));
    for (uint32 i = 0; i < static_cast<uint32>(ctx.renderPasses.size()); ++i)
    {
        const RHIRenderPassDesc& renderPass = ctx.renderPasses[i];
        EXPECT_EQ(renderPass.colorAttachmentCount, 0u);
        EXPECT_TRUE(renderPass.hasDepthStencil);
        ASSERT_NE(renderPass.depthStencilAttachment.view, nullptr);
        EXPECT_EQ(renderPass.depthStencilAttachment.view->GetSubresourceRange().baseArrayLayer, i);
        EXPECT_EQ(renderPass.depthStencilAttachment.view->GetSubresourceRange().arrayLayerCount, 1u);
        EXPECT_EQ(renderPass.depthStencilAttachment.view->GetSubresourceRange().aspect, RHITextureAspect::Depth);
    }
}

TEST_F(RenderPassValidationFixture, SkyboxPassWithoutSelectedSkyboxDoesNotBindOrDraw)
{
    ASSERT_NO_FATAL_FAILURE(Initialize());

    SkyboxPass pass;
    pass.SetResources(&pipelineCache);
    pass.SetRenderTargets(colorView.Get(), nullptr);

    EXPECT_TRUE(pass.IsRequestedEnabled());
    EXPECT_FALSE(pass.IsSupported());
    EXPECT_FALSE(pass.IsEnabled());
    EXPECT_FALSE(pass.GetUnsupportedReason().empty());

    RecordingCommandContext ctx;
    pass.Execute(ctx, view);

    EXPECT_EQ(ctx.beginRenderPassCount, 0u);
    EXPECT_TRUE(ctx.pipelineSequence.empty());
    EXPECT_EQ(ctx.drawCount, 0u);
}

TEST_F(RenderPassValidationFixture, SkyboxPassDrawsProceduralFullscreenTriangleThroughRenderGraph)
{
    ASSERT_NO_FATAL_FAILURE(Initialize());

    RenderGraph graph;
    graph.SetDevice(&device);

    RHITextureDesc sceneColorDesc = RHITextureDesc::RenderTarget(64, 64, RHIFormat::RGBA8_UNORM);
    sceneColorDesc.debugName = "GraphSceneColorForSkyboxPass";
    view.colorTarget = graph.CreateTexture(sceneColorDesc);
    graph.SetExportState(view.colorTarget, RHIResourceState::RenderTarget);

    RHITextureDesc depthDesc = RHITextureDesc::DepthStencil(64, 64, PipelineCache::GetDefaultDepthStencilFormat());
    depthDesc.debugName = "GraphDepthForSkyboxPass";
    view.depthTarget = graph.CreateTexture(depthDesc);
    graph.SetExportState(view.depthTarget, RHIResourceState::DepthRead);

    view.renderGraph = &graph;
    view.viewCache = &viewCache;
    view.viewportWidth = 64;
    view.viewportHeight = 64;

    SkyboxPass pass;
    pass.SetResources(&pipelineCache);
    pass.SetRenderTargets(colorView.Get(), nullptr);
    pass.SetProceduralSkyParams(Vec3{0.25f, 0.8f, 0.35f},
                                Vec3{0.12f, 0.24f, 0.55f},
                                Vec3{0.6f, 0.72f, 0.88f});

    ASSERT_TRUE(pass.IsSupported()) << pass.GetUnsupportedReason();
    ASSERT_TRUE(pass.IsEnabled());

    struct SkyboxTestData
    {
    };

    graph.AddPass<SkyboxTestData>(
        "SkyboxPassTest",
        RenderGraphPassType::Graphics,
        [this, &pass](RenderGraphBuilder& builder, SkyboxTestData&)
        {
            pass.Setup(builder, view);
        },
        [this, &pass](const SkyboxTestData&, RHICommandContext& ctx)
        {
            pass.Execute(ctx, view);
        });

    graph.Compile();
    RecordingCommandContext ctx;
    graph.Execute(ctx);

    ASSERT_EQ(ctx.beginRenderPassCount, 1u);
    ASSERT_EQ(ctx.endRenderPassCount, 1u);
    ASSERT_EQ(ctx.pipelineSequence.size(), static_cast<size_t>(1));
    EXPECT_EQ(ctx.pipelineSequence[0], pipelineCache.GetSkyboxPipeline());
    ASSERT_EQ(ctx.descriptorSetSequence.size(), static_cast<size_t>(1));
    EXPECT_EQ(ctx.descriptorSetSequence[0], 0u);
    EXPECT_EQ(ctx.drawCount, 1u);
    EXPECT_EQ(ctx.lastDrawVertexCount, 3u);

    ASSERT_FALSE(ctx.renderPasses.empty());
    ASSERT_GT(ctx.renderPasses[0].colorAttachmentCount, 0u);
    RHITextureView* resolvedColorView = ctx.renderPasses[0].colorAttachments[0].view;
    ASSERT_NE(resolvedColorView, nullptr);
    EXPECT_NE(resolvedColorView, colorView.Get());
    EXPECT_TRUE(ctx.renderPasses[0].hasDepthStencil);
    ASSERT_NE(ctx.renderPasses[0].depthStencilAttachment.view, nullptr);

    auto descriptorIt = std::find_if(
        device.createdDescriptorSetDescs.begin(),
        device.createdDescriptorSetDescs.end(),
        [](const RHIDescriptorSetDesc& desc)
        {
            return desc.debugName && std::string(desc.debugName) == "SkyboxDescriptorSet";
        });
    ASSERT_NE(descriptorIt, device.createdDescriptorSetDescs.end());
    ASSERT_EQ(descriptorIt->bindings.size(), static_cast<size_t>(3));
    EXPECT_EQ(descriptorIt->bindings[0].binding, 0u);
    EXPECT_NE(descriptorIt->bindings[0].buffer, nullptr);
    EXPECT_EQ(descriptorIt->bindings[1].binding, 1u);
    EXPECT_NE(descriptorIt->bindings[1].textureView, nullptr);
    EXPECT_EQ(descriptorIt->bindings[1].textureView->GetTexture()->GetDimension(), RHITextureDimension::TextureCube);
    EXPECT_EQ(descriptorIt->bindings[2].binding, 2u);
    EXPECT_NE(descriptorIt->bindings[2].sampler, nullptr);
}

TEST_F(RenderPassValidationFixture, SkyboxPassDrawsFullscreenBackgroundWithoutDepthTarget)
{
    ASSERT_NO_FATAL_FAILURE(Initialize());

    view.viewportWidth = 64;
    view.viewportHeight = 64;

    SkyboxPass pass;
    pass.SetResources(&pipelineCache);
    pass.SetRenderTargets(colorView.Get(), nullptr);
    pass.SetProceduralSkyParams(Vec3{0.25f, 0.8f, 0.35f},
                                Vec3{0.12f, 0.24f, 0.55f},
                                Vec3{0.6f, 0.72f, 0.88f});

    RecordingCommandContext ctx;
    pass.Execute(ctx, view);

    ASSERT_EQ(ctx.beginRenderPassCount, 1u);
    ASSERT_EQ(ctx.pipelineSequence.size(), static_cast<size_t>(1));
    EXPECT_EQ(ctx.pipelineSequence[0], pipelineCache.GetSkyboxPipeline(RHIFormat::RGBA8_UNORM, false));
    EXPECT_EQ(ctx.drawCount, 1u);
    EXPECT_EQ(ctx.lastDrawVertexCount, 3u);
    ASSERT_FALSE(ctx.renderPasses.empty());
    EXPECT_FALSE(ctx.renderPasses[0].hasDepthStencil);
}

TEST_F(RenderPassValidationFixture, SkyboxPassDrawsCubemapFullscreenTriangle)
{
    ASSERT_NO_FATAL_FAILURE(Initialize());

    view.viewportWidth = 64;
    view.viewportHeight = 64;

    RHITextureDesc cubemapDesc = RHITextureDesc::Texture2D(16, 16, RHIFormat::RGBA8_UNORM);
    cubemapDesc.dimension = RHITextureDimension::TextureCube;
    cubemapDesc.arraySize = 1;
    auto cubemap = device.CreateTexture(cubemapDesc);
    ASSERT_NE(cubemap, nullptr);

    SkyboxPass pass;
    pass.SetResources(&pipelineCache);
    pass.SetRenderTargets(colorView.Get(), nullptr);
    pass.SetCubemap(cubemap.Get(), 1.25f, 0.35f, 1.0f);

    ASSERT_TRUE(pass.IsSupported()) << pass.GetUnsupportedReason();
    ASSERT_TRUE(pass.IsCubemapSelected());
    EXPECT_EQ(pass.GetSelectedCubemap(), cubemap.Get());

    RecordingCommandContext ctx;
    pass.Execute(ctx, view);

    ASSERT_EQ(ctx.beginRenderPassCount, 1u);
    ASSERT_EQ(ctx.pipelineSequence.size(), static_cast<size_t>(1));
    EXPECT_EQ(ctx.pipelineSequence[0], pipelineCache.GetSkyboxPipeline(RHIFormat::RGBA8_UNORM, false));
    EXPECT_EQ(ctx.drawCount, 1u);
    EXPECT_EQ(ctx.lastDrawVertexCount, 3u);

    auto descriptorIt = std::find_if(
        device.createdDescriptorSetDescs.begin(),
        device.createdDescriptorSetDescs.end(),
        [](const RHIDescriptorSetDesc& desc)
        {
            return desc.debugName && std::string(desc.debugName) == "SkyboxDescriptorSet";
        });
    ASSERT_NE(descriptorIt, device.createdDescriptorSetDescs.end());
    ASSERT_EQ(descriptorIt->bindings.size(), static_cast<size_t>(3));
    EXPECT_EQ(descriptorIt->bindings[0].binding, 0u);
    EXPECT_NE(descriptorIt->bindings[0].buffer, nullptr);
    EXPECT_EQ(descriptorIt->bindings[1].binding, 1u);
    ASSERT_NE(descriptorIt->bindings[1].textureView, nullptr);
    EXPECT_EQ(descriptorIt->bindings[1].textureView->GetTexture(), cubemap.Get());
    EXPECT_EQ(descriptorIt->bindings[2].binding, 2u);
    EXPECT_NE(descriptorIt->bindings[2].sampler, nullptr);
}

TEST_F(RenderPassValidationFixture, SkyboxPassSkipsCubemapDrawWhenSRVCreationFails)
{
    ASSERT_NO_FATAL_FAILURE(Initialize());

    RHITextureDesc cubemapDesc = RHITextureDesc::Texture2D(16, 16, RHIFormat::RGBA8_UNORM);
    cubemapDesc.dimension = RHITextureDimension::TextureCube;
    cubemapDesc.arraySize = 1;
    auto cubemap = device.CreateTexture(cubemapDesc);
    ASSERT_NE(cubemap, nullptr);

    SkyboxPass pass;
    pass.SetResources(&pipelineCache);
    pass.SetRenderTargets(colorView.Get(), nullptr);
    pass.SetCubemap(cubemap.Get());
    ASSERT_TRUE(pass.IsSupported()) << pass.GetUnsupportedReason();

    device.textureViewCreationSucceeds = false;

    RecordingCommandContext ctx;
    pass.Execute(ctx, view);

    EXPECT_TRUE(ctx.pipelineSequence.empty());
    EXPECT_EQ(ctx.drawCount, 0u);
}

TEST_F(RenderPassValidationFixture, SkyboxPassSkipsDrawWhenSamplerCannotBeCreated)
{
    ASSERT_NO_FATAL_FAILURE(Initialize());

    device.samplerCreationSucceeds = false;

    SkyboxPass pass;
    pass.SetResources(&pipelineCache);
    pass.SetRenderTargets(colorView.Get(), nullptr);
    pass.SetProceduralSkyParams(Vec3{0.25f, 0.8f, 0.35f},
                                Vec3{0.12f, 0.24f, 0.55f},
                                Vec3{0.6f, 0.72f, 0.88f});

    EXPECT_FALSE(pass.IsSupported());

    RecordingCommandContext ctx;
    pass.Execute(ctx, view);

    EXPECT_TRUE(ctx.pipelineSequence.empty());
    EXPECT_EQ(ctx.drawCount, 0u);
}

TEST_F(RenderPassValidationFixture, SkyboxPassSkipsDrawWhenConstantsCannotMap)
{
    ASSERT_NO_FATAL_FAILURE(Initialize(false));

    SkyboxPass pass;
    pass.SetResources(&pipelineCache);
    pass.SetRenderTargets(colorView.Get(), nullptr);
    pass.SetProceduralSkyParams(Vec3{0.25f, 0.8f, 0.35f},
                                Vec3{0.12f, 0.24f, 0.55f},
                                Vec3{0.6f, 0.72f, 0.88f});

    RecordingCommandContext ctx;
    pass.Execute(ctx, view);

    EXPECT_TRUE(ctx.pipelineSequence.empty());
    EXPECT_EQ(ctx.drawCount, 0u);
}

TEST_F(RenderPassValidationFixture, ToneMappingRequiresResourcesBeforeReportingSupported)
{
    ToneMappingPass pass;
    PostProcessSettings settings;
    settings.enableToneMapping = true;
    pass.Configure(settings);

    EXPECT_TRUE(pass.IsRequestedEnabled());
    EXPECT_FALSE(pass.IsSupported());
    EXPECT_FALSE(pass.IsEnabled());
    EXPECT_FALSE(pass.GetUnsupportedReason().empty());
}

TEST_F(RenderPassValidationFixture, ToneMappingAddsLiveGraphPassAndDrawsFullscreenTriangle)
{
    ASSERT_NO_FATAL_FAILURE(Initialize());

    ToneMappingPass pass;
    PostProcessSettings settings;
    settings.enableToneMapping = true;
    settings.exposure = 1.25f;
    settings.gamma = 2.2f;
    pass.Configure(settings);
    pass.SetResources(&pipelineCache, &viewCache);

    ASSERT_TRUE(pass.IsSupported()) << pass.GetUnsupportedReason();
    ASSERT_TRUE(pass.IsEnabled());

    RenderGraph graph;
    graph.SetDevice(&device);

    RHITextureRef inputTexture =
        device.CreateTexture(RHITextureDesc::RenderTarget(64, 64, RHIFormat::RGBA16_FLOAT));
    RHITextureRef outputTexture =
        device.CreateTexture(RHITextureDesc::RenderTarget(64, 64, RHIFormat::RGBA8_UNORM));
    ASSERT_TRUE(inputTexture);
    ASSERT_TRUE(outputTexture);

    RGTextureHandle input = graph.ImportTexture(inputTexture.Get(), RHIResourceState::ShaderResource);
    RGTextureHandle output = graph.ImportTexture(outputTexture.Get(), RHIResourceState::RenderTarget);
    graph.SetExportState(output, RHIResourceState::RenderTarget);

    pass.AddToGraph(graph, input, output);
    graph.Compile();

    const auto& stats = graph.GetCompileStats();
    EXPECT_TRUE(stats.compileValid);
    EXPECT_EQ(stats.totalPasses, 1u);
    EXPECT_EQ(stats.culledPasses, 0u);
    EXPECT_EQ(stats.emptyPassUsageCount, 0u);

    RecordingCommandContext ctx;
    graph.Execute(ctx);

    EXPECT_EQ(ctx.beginRenderPassCount, 1u);
    EXPECT_EQ(ctx.endRenderPassCount, 1u);
    ASSERT_EQ(ctx.pipelineSequence.size(), static_cast<size_t>(1));
    EXPECT_EQ(ctx.pipelineSequence[0], pipelineCache.GetToneMappingPipeline());
    ASSERT_EQ(ctx.descriptorSetSequence.size(), static_cast<size_t>(1));
    EXPECT_EQ(ctx.descriptorSetSequence[0], 0u);
    EXPECT_EQ(ctx.drawCount, 1u);
    EXPECT_EQ(ctx.lastDrawVertexCount, 3u);
    EXPECT_EQ(ctx.drawIndexedCount, 0u);

    ASSERT_EQ(ctx.renderPasses.size(), static_cast<size_t>(1));
    EXPECT_EQ(ctx.renderPasses[0].colorAttachmentCount, 1u);
    EXPECT_FALSE(ctx.renderPasses[0].hasDepthStencil);
    EXPECT_EQ(ctx.renderPasses[0].renderArea.width, 64u);
    EXPECT_EQ(ctx.renderPasses[0].renderArea.height, 64u);

    ASSERT_FALSE(ctx.viewports.empty());
    EXPECT_EQ(ctx.viewports.back().width, 64.0f);
    EXPECT_EQ(ctx.viewports.back().height, 64.0f);
    ASSERT_FALSE(ctx.scissors.empty());
    EXPECT_EQ(ctx.scissors.back().width, 64u);
    EXPECT_EQ(ctx.scissors.back().height, 64u);

    auto descriptorIt = std::find_if(
        device.createdDescriptorSetDescs.begin(),
        device.createdDescriptorSetDescs.end(),
        [](const RHIDescriptorSetDesc& desc)
        {
            return desc.debugName && std::string(desc.debugName) == "ToneMappingDescriptorSet";
        });
    ASSERT_NE(descriptorIt, device.createdDescriptorSetDescs.end());
    EXPECT_EQ(descriptorIt->layout, pipelineCache.GetPostProcessSetLayout());
    ASSERT_EQ(descriptorIt->bindings.size(), static_cast<size_t>(3));

    const auto hasBinding = [descriptorIt](uint32 binding,
                                           bool expectBuffer,
                                           bool expectTexture,
                                           bool expectSampler)
    {
        auto it = std::find_if(descriptorIt->bindings.begin(),
                               descriptorIt->bindings.end(),
                               [binding](const RHIDescriptorBinding& descriptorBinding)
                               {
                                   return descriptorBinding.binding == binding;
                               });
        if (it == descriptorIt->bindings.end())
            return false;

        return (it->buffer != nullptr) == expectBuffer &&
               (it->textureView != nullptr) == expectTexture &&
               (it->sampler != nullptr) == expectSampler;
    };

    EXPECT_TRUE(hasBinding(0, true, false, false));
    EXPECT_TRUE(hasBinding(1, false, true, false));
    EXPECT_TRUE(hasBinding(2, false, false, true));

    auto descriptorCall = std::find(ctx.callSequence.begin(), ctx.callSequence.end(), "SetDescriptorSet");
    auto drawCall = std::find(ctx.callSequence.begin(), ctx.callSequence.end(), "Draw");
    ASSERT_NE(descriptorCall, ctx.callSequence.end());
    ASSERT_NE(drawCall, ctx.callSequence.end());
    EXPECT_LT(std::distance(ctx.callSequence.begin(), descriptorCall),
              std::distance(ctx.callSequence.begin(), drawCall));
}

TEST_F(RenderPassValidationFixture, ToneMappingSkipsDrawWhenConstantsCannotMap)
{
    ASSERT_NO_FATAL_FAILURE(Initialize(false));

    ToneMappingPass pass;
    PostProcessSettings settings;
    settings.enableToneMapping = true;
    pass.Configure(settings);
    pass.SetResources(&pipelineCache, &viewCache);

    ASSERT_TRUE(pass.IsSupported()) << pass.GetUnsupportedReason();

    RenderGraph graph;
    graph.SetDevice(&device);

    RHITextureRef inputTexture =
        device.CreateTexture(RHITextureDesc::RenderTarget(16, 16, RHIFormat::RGBA16_FLOAT));
    RHITextureRef outputTexture =
        device.CreateTexture(RHITextureDesc::RenderTarget(16, 16, RHIFormat::RGBA8_UNORM));
    ASSERT_TRUE(inputTexture);
    ASSERT_TRUE(outputTexture);

    RGTextureHandle input = graph.ImportTexture(inputTexture.Get(), RHIResourceState::ShaderResource);
    RGTextureHandle output = graph.ImportTexture(outputTexture.Get(), RHIResourceState::RenderTarget);
    graph.SetExportState(output, RHIResourceState::RenderTarget);

    pass.AddToGraph(graph, input, output);
    graph.Compile();

    RecordingCommandContext ctx;
    graph.Execute(ctx);

    EXPECT_EQ(ctx.drawCount, 0u);
    EXPECT_EQ(ctx.beginRenderPassCount, 0u);
}

TEST_F(RenderPassValidationFixture, BloomRequiresResourcesBeforeReportingSupported)
{
    BloomPass pass;
    PostProcessSettings settings;
    settings.enableBloom = true;
    pass.Configure(settings);

    EXPECT_TRUE(pass.IsRequestedEnabled());
    EXPECT_FALSE(pass.IsSupported());
    EXPECT_FALSE(pass.IsEnabled());
    EXPECT_FALSE(pass.GetUnsupportedReason().empty());
}

TEST_F(RenderPassValidationFixture, BloomAddsLiveGraphPassAndDrawsFullscreenTriangle)
{
    ASSERT_NO_FATAL_FAILURE(Initialize());

    BloomPass pass;
    PostProcessSettings settings;
    settings.enableBloom = true;
    settings.bloomThreshold = 1.1f;
    settings.bloomIntensity = 0.75f;
    settings.bloomRadius = 1.5f;
    pass.Configure(settings);
    pass.SetResources(&pipelineCache, &viewCache);

    ASSERT_TRUE(pass.IsSupported()) << pass.GetUnsupportedReason();
    ASSERT_TRUE(pass.IsEnabled());

    RenderGraph graph;
    graph.SetDevice(&device);

    RHITextureRef inputTexture =
        device.CreateTexture(RHITextureDesc::RenderTarget(64, 64, RHIFormat::RGBA16_FLOAT));
    RHITextureRef outputTexture =
        device.CreateTexture(RHITextureDesc::RenderTarget(64, 64, RHIFormat::RGBA16_FLOAT));
    ASSERT_TRUE(inputTexture);
    ASSERT_TRUE(outputTexture);

    RGTextureHandle input = graph.ImportTexture(inputTexture.Get(), RHIResourceState::ShaderResource);
    RGTextureHandle output = graph.ImportTexture(outputTexture.Get(), RHIResourceState::RenderTarget);
    graph.SetExportState(output, RHIResourceState::RenderTarget);

    pass.AddToGraph(graph, input, output);
    graph.Compile();

    const auto& stats = graph.GetCompileStats();
    EXPECT_TRUE(stats.compileValid);
    EXPECT_EQ(stats.totalPasses, 1u);
    EXPECT_EQ(stats.culledPasses, 0u);
    EXPECT_EQ(stats.emptyPassUsageCount, 0u);

    RecordingCommandContext ctx;
    graph.Execute(ctx);

    EXPECT_EQ(ctx.beginRenderPassCount, 1u);
    EXPECT_EQ(ctx.endRenderPassCount, 1u);
    ASSERT_EQ(ctx.pipelineSequence.size(), static_cast<size_t>(1));
    EXPECT_EQ(ctx.pipelineSequence[0], pipelineCache.GetBloomPipeline(RHIFormat::RGBA16_FLOAT));
    ASSERT_EQ(ctx.descriptorSetSequence.size(), static_cast<size_t>(1));
    EXPECT_EQ(ctx.descriptorSetSequence[0], 0u);
    EXPECT_EQ(ctx.drawCount, 1u);
    EXPECT_EQ(ctx.lastDrawVertexCount, 3u);
    EXPECT_EQ(ctx.drawIndexedCount, 0u);

    ASSERT_EQ(ctx.renderPasses.size(), static_cast<size_t>(1));
    EXPECT_EQ(ctx.renderPasses[0].colorAttachmentCount, 1u);
    EXPECT_FALSE(ctx.renderPasses[0].hasDepthStencil);
    EXPECT_EQ(ctx.renderPasses[0].renderArea.width, 64u);
    EXPECT_EQ(ctx.renderPasses[0].renderArea.height, 64u);

    ASSERT_FALSE(ctx.viewports.empty());
    EXPECT_EQ(ctx.viewports.back().width, 64.0f);
    EXPECT_EQ(ctx.viewports.back().height, 64.0f);
    ASSERT_FALSE(ctx.scissors.empty());
    EXPECT_EQ(ctx.scissors.back().width, 64u);
    EXPECT_EQ(ctx.scissors.back().height, 64u);

    auto descriptorIt = std::find_if(
        device.createdDescriptorSetDescs.begin(),
        device.createdDescriptorSetDescs.end(),
        [](const RHIDescriptorSetDesc& desc)
        {
            return desc.debugName && std::string(desc.debugName) == "BloomDescriptorSet";
        });
    ASSERT_NE(descriptorIt, device.createdDescriptorSetDescs.end());
    EXPECT_EQ(descriptorIt->layout, pipelineCache.GetPostProcessSetLayout());
    ASSERT_EQ(descriptorIt->bindings.size(), static_cast<size_t>(3));

    const auto hasBinding = [descriptorIt](uint32 binding,
                                           bool expectBuffer,
                                           bool expectTexture,
                                           bool expectSampler)
    {
        auto it = std::find_if(descriptorIt->bindings.begin(),
                               descriptorIt->bindings.end(),
                               [binding](const RHIDescriptorBinding& descriptorBinding)
                               {
                                   return descriptorBinding.binding == binding;
                               });
        if (it == descriptorIt->bindings.end())
            return false;

        return (it->buffer != nullptr) == expectBuffer &&
               (it->textureView != nullptr) == expectTexture &&
               (it->sampler != nullptr) == expectSampler;
    };

    EXPECT_TRUE(hasBinding(0, true, false, false));
    EXPECT_TRUE(hasBinding(1, false, true, false));
    EXPECT_TRUE(hasBinding(2, false, false, true));

    auto descriptorCall = std::find(ctx.callSequence.begin(), ctx.callSequence.end(), "SetDescriptorSet");
    auto drawCall = std::find(ctx.callSequence.begin(), ctx.callSequence.end(), "Draw");
    ASSERT_NE(descriptorCall, ctx.callSequence.end());
    ASSERT_NE(drawCall, ctx.callSequence.end());
    EXPECT_LT(std::distance(ctx.callSequence.begin(), descriptorCall),
              std::distance(ctx.callSequence.begin(), drawCall));
}

TEST_F(RenderPassValidationFixture, BloomSkipsDrawWhenConstantsCannotMap)
{
    ASSERT_NO_FATAL_FAILURE(Initialize(false));

    BloomPass pass;
    PostProcessSettings settings;
    settings.enableBloom = true;
    pass.Configure(settings);
    pass.SetResources(&pipelineCache, &viewCache);

    ASSERT_TRUE(pass.IsSupported()) << pass.GetUnsupportedReason();

    RenderGraph graph;
    graph.SetDevice(&device);

    RHITextureRef inputTexture =
        device.CreateTexture(RHITextureDesc::RenderTarget(16, 16, RHIFormat::RGBA16_FLOAT));
    RHITextureRef outputTexture =
        device.CreateTexture(RHITextureDesc::RenderTarget(16, 16, RHIFormat::RGBA16_FLOAT));
    ASSERT_TRUE(inputTexture);
    ASSERT_TRUE(outputTexture);

    RGTextureHandle input = graph.ImportTexture(inputTexture.Get(), RHIResourceState::ShaderResource);
    RGTextureHandle output = graph.ImportTexture(outputTexture.Get(), RHIResourceState::RenderTarget);
    graph.SetExportState(output, RHIResourceState::RenderTarget);

    pass.AddToGraph(graph, input, output);
    graph.Compile();

    RecordingCommandContext ctx;
    graph.Execute(ctx);

    EXPECT_EQ(ctx.drawCount, 0u);
    EXPECT_EQ(ctx.beginRenderPassCount, 0u);
}

TEST_F(RenderPassValidationFixture, FXAARequiresResourcesBeforeReportingSupported)
{
    FXAAPass pass;
    PostProcessSettings settings;
    settings.enableFXAA = true;
    pass.Configure(settings);

    EXPECT_TRUE(pass.IsRequestedEnabled());
    EXPECT_FALSE(pass.IsSupported());
    EXPECT_FALSE(pass.IsEnabled());
    EXPECT_FALSE(pass.GetUnsupportedReason().empty());
}

TEST_F(RenderPassValidationFixture, FXAAAddsLiveGraphPassAndDrawsFullscreenTriangle)
{
    ASSERT_NO_FATAL_FAILURE(Initialize());

    FXAAPass pass;
    PostProcessSettings settings;
    settings.enableFXAA = true;
    settings.fxaaQuality = 0.85f;
    pass.Configure(settings);
    pass.SetResources(&pipelineCache, &viewCache);

    ASSERT_TRUE(pass.IsSupported()) << pass.GetUnsupportedReason();
    ASSERT_TRUE(pass.IsEnabled());

    RenderGraph graph;
    graph.SetDevice(&device);

    RHITextureRef inputTexture =
        device.CreateTexture(RHITextureDesc::RenderTarget(64, 64, RHIFormat::RGBA8_UNORM));
    RHITextureRef outputTexture =
        device.CreateTexture(RHITextureDesc::RenderTarget(64, 64, RHIFormat::RGBA8_UNORM));
    ASSERT_TRUE(inputTexture);
    ASSERT_TRUE(outputTexture);

    RGTextureHandle input = graph.ImportTexture(inputTexture.Get(), RHIResourceState::ShaderResource);
    RGTextureHandle output = graph.ImportTexture(outputTexture.Get(), RHIResourceState::RenderTarget);
    graph.SetExportState(output, RHIResourceState::RenderTarget);

    pass.AddToGraph(graph, input, output);
    graph.Compile();

    const auto& stats = graph.GetCompileStats();
    EXPECT_TRUE(stats.compileValid);
    EXPECT_EQ(stats.totalPasses, 1u);
    EXPECT_EQ(stats.culledPasses, 0u);
    EXPECT_EQ(stats.emptyPassUsageCount, 0u);

    RecordingCommandContext ctx;
    graph.Execute(ctx);

    EXPECT_EQ(ctx.beginRenderPassCount, 1u);
    EXPECT_EQ(ctx.endRenderPassCount, 1u);
    ASSERT_EQ(ctx.pipelineSequence.size(), static_cast<size_t>(1));
    EXPECT_EQ(ctx.pipelineSequence[0], pipelineCache.GetFXAAPipeline(RHIFormat::RGBA8_UNORM));
    ASSERT_EQ(ctx.descriptorSetSequence.size(), static_cast<size_t>(1));
    EXPECT_EQ(ctx.descriptorSetSequence[0], 0u);
    EXPECT_EQ(ctx.drawCount, 1u);
    EXPECT_EQ(ctx.lastDrawVertexCount, 3u);
    EXPECT_EQ(ctx.drawIndexedCount, 0u);

    ASSERT_EQ(ctx.renderPasses.size(), static_cast<size_t>(1));
    EXPECT_EQ(ctx.renderPasses[0].colorAttachmentCount, 1u);
    EXPECT_FALSE(ctx.renderPasses[0].hasDepthStencil);
    EXPECT_EQ(ctx.renderPasses[0].renderArea.width, 64u);
    EXPECT_EQ(ctx.renderPasses[0].renderArea.height, 64u);

    ASSERT_FALSE(ctx.viewports.empty());
    EXPECT_EQ(ctx.viewports.back().width, 64.0f);
    EXPECT_EQ(ctx.viewports.back().height, 64.0f);
    ASSERT_FALSE(ctx.scissors.empty());
    EXPECT_EQ(ctx.scissors.back().width, 64u);
    EXPECT_EQ(ctx.scissors.back().height, 64u);

    auto descriptorIt = std::find_if(
        device.createdDescriptorSetDescs.begin(),
        device.createdDescriptorSetDescs.end(),
        [](const RHIDescriptorSetDesc& desc)
        {
            return desc.debugName && std::string(desc.debugName) == "FXAADescriptorSet";
        });
    ASSERT_NE(descriptorIt, device.createdDescriptorSetDescs.end());
    EXPECT_EQ(descriptorIt->layout, pipelineCache.GetPostProcessSetLayout());
    ASSERT_EQ(descriptorIt->bindings.size(), static_cast<size_t>(3));

    const auto hasBinding = [descriptorIt](uint32 binding,
                                           bool expectBuffer,
                                           bool expectTexture,
                                           bool expectSampler)
    {
        auto it = std::find_if(descriptorIt->bindings.begin(),
                               descriptorIt->bindings.end(),
                               [binding](const RHIDescriptorBinding& descriptorBinding)
                               {
                                   return descriptorBinding.binding == binding;
                               });
        if (it == descriptorIt->bindings.end())
            return false;

        return (it->buffer != nullptr) == expectBuffer &&
               (it->textureView != nullptr) == expectTexture &&
               (it->sampler != nullptr) == expectSampler;
    };

    EXPECT_TRUE(hasBinding(0, true, false, false));
    EXPECT_TRUE(hasBinding(1, false, true, false));
    EXPECT_TRUE(hasBinding(2, false, false, true));

    auto descriptorCall = std::find(ctx.callSequence.begin(), ctx.callSequence.end(), "SetDescriptorSet");
    auto drawCall = std::find(ctx.callSequence.begin(), ctx.callSequence.end(), "Draw");
    ASSERT_NE(descriptorCall, ctx.callSequence.end());
    ASSERT_NE(drawCall, ctx.callSequence.end());
    EXPECT_LT(std::distance(ctx.callSequence.begin(), descriptorCall),
              std::distance(ctx.callSequence.begin(), drawCall));
}

TEST_F(RenderPassValidationFixture, FXAASkipsDrawWhenConstantsCannotMap)
{
    ASSERT_NO_FATAL_FAILURE(Initialize(false));

    FXAAPass pass;
    PostProcessSettings settings;
    settings.enableFXAA = true;
    pass.Configure(settings);
    pass.SetResources(&pipelineCache, &viewCache);

    ASSERT_TRUE(pass.IsSupported()) << pass.GetUnsupportedReason();

    RenderGraph graph;
    graph.SetDevice(&device);

    RHITextureRef inputTexture =
        device.CreateTexture(RHITextureDesc::RenderTarget(16, 16, RHIFormat::RGBA8_UNORM));
    RHITextureRef outputTexture =
        device.CreateTexture(RHITextureDesc::RenderTarget(16, 16, RHIFormat::RGBA8_UNORM));
    ASSERT_TRUE(inputTexture);
    ASSERT_TRUE(outputTexture);

    RGTextureHandle input = graph.ImportTexture(inputTexture.Get(), RHIResourceState::ShaderResource);
    RGTextureHandle output = graph.ImportTexture(outputTexture.Get(), RHIResourceState::RenderTarget);
    graph.SetExportState(output, RHIResourceState::RenderTarget);

    pass.AddToGraph(graph, input, output);
    graph.Compile();

    RecordingCommandContext ctx;
    graph.Execute(ctx);

    EXPECT_EQ(ctx.drawCount, 0u);
    EXPECT_EQ(ctx.beginRenderPassCount, 0u);
}

TEST_F(RenderPassValidationFixture, ColorGradingRequiresResourcesBeforeReportingSupported)
{
    ColorGradingPass pass;
    PostProcessSettings settings;
    settings.enableColorGrading = true;
    pass.Configure(settings);

    EXPECT_TRUE(pass.IsRequestedEnabled());
    EXPECT_EQ(pass.GetMode(), ColorGradingMode::LDR);
    EXPECT_FALSE(pass.IsSupported());
    EXPECT_FALSE(pass.IsEnabled());
    EXPECT_FALSE(pass.GetUnsupportedReason().empty());
}

TEST_F(RenderPassValidationFixture, ColorGradingDefaultConfiguredPathIsLDRAndSupportedWithResources)
{
    ASSERT_NO_FATAL_FAILURE(Initialize());

    ColorGradingPass pass;
    PostProcessSettings settings;
    settings.enableColorGrading = true;
    pass.Configure(settings);
    pass.SetResources(&pipelineCache, &viewCache);

    EXPECT_EQ(pass.GetMode(), ColorGradingMode::LDR);
    EXPECT_TRUE(pass.IsSupported()) << pass.GetUnsupportedReason();
    EXPECT_TRUE(pass.IsEnabled());
}

TEST_F(RenderPassValidationFixture, ColorGradingRejectsHDRModeBeforeGraphScheduling)
{
    ASSERT_NO_FATAL_FAILURE(Initialize());

    ColorGradingPass pass;
    PostProcessSettings settings;
    settings.enableColorGrading = true;
    pass.Configure(settings);
    pass.SetResources(&pipelineCache, &viewCache);
    pass.SetMode(ColorGradingMode::HDR);

    EXPECT_TRUE(pass.IsRequestedEnabled());
    EXPECT_FALSE(pass.IsSupported());
    EXPECT_FALSE(pass.IsEnabled());
    EXPECT_NE(pass.GetUnsupportedReason().find("HDR"), std::string::npos);

    RenderGraph graph;
    graph.SetDevice(&device);

    RHITextureRef inputTexture =
        device.CreateTexture(RHITextureDesc::RenderTarget(16, 16, RHIFormat::RGBA8_UNORM));
    RHITextureRef outputTexture =
        device.CreateTexture(RHITextureDesc::RenderTarget(16, 16, RHIFormat::RGBA8_UNORM));
    ASSERT_TRUE(inputTexture);
    ASSERT_TRUE(outputTexture);

    RGTextureHandle input = graph.ImportTexture(inputTexture.Get(), RHIResourceState::ShaderResource);
    RGTextureHandle output = graph.ImportTexture(outputTexture.Get(), RHIResourceState::RenderTarget);
    graph.SetExportState(output, RHIResourceState::RenderTarget);

    pass.AddToGraph(graph, input, output);
    graph.Compile();
    EXPECT_TRUE(graph.GetCompileStats().compileValid);
    EXPECT_EQ(graph.GetCompileStats().totalPasses, 0u);

    RecordingCommandContext ctx;
    graph.Execute(ctx);
    EXPECT_EQ(ctx.drawCount, 0u);
    EXPECT_EQ(ctx.beginRenderPassCount, 0u);
}

TEST_F(RenderPassValidationFixture, ColorGradingRejectsRequestedLUTBeforeGraphScheduling)
{
    ASSERT_NO_FATAL_FAILURE(Initialize());

    ColorGradingPass pass;
    PostProcessSettings settings;
    settings.enableColorGrading = true;
    pass.Configure(settings);
    pass.SetResources(&pipelineCache, &viewCache);
    pass.SetUseLUT(true);

    EXPECT_FALSE(pass.IsSupported());
    EXPECT_FALSE(pass.IsEnabled());
    EXPECT_NE(pass.GetUnsupportedReason().find("LUT"), std::string::npos);

    RenderGraph graph;
    graph.SetDevice(&device);
    RHITextureRef inputTexture =
        device.CreateTexture(RHITextureDesc::RenderTarget(16, 16, RHIFormat::RGBA8_UNORM));
    RHITextureRef outputTexture =
        device.CreateTexture(RHITextureDesc::RenderTarget(16, 16, RHIFormat::RGBA8_UNORM));
    ASSERT_TRUE(inputTexture);
    ASSERT_TRUE(outputTexture);

    RGTextureHandle input = graph.ImportTexture(inputTexture.Get(), RHIResourceState::ShaderResource);
    RGTextureHandle output = graph.ImportTexture(outputTexture.Get(), RHIResourceState::RenderTarget);
    graph.SetExportState(output, RHIResourceState::RenderTarget);

    pass.AddToGraph(graph, input, output);
    graph.Compile();
    EXPECT_TRUE(graph.GetCompileStats().compileValid);
    EXPECT_EQ(graph.GetCompileStats().totalPasses, 0u);
}

TEST_F(RenderPassValidationFixture, ColorGradingRejectsSetLUTBeforeGraphScheduling)
{
    ASSERT_NO_FATAL_FAILURE(Initialize());

    ColorGradingPass pass;
    PostProcessSettings settings;
    settings.enableColorGrading = true;
    pass.Configure(settings);
    pass.SetResources(&pipelineCache, &viewCache);

    RHITextureRef lutTexture =
        device.CreateTexture(RHITextureDesc::Texture2D(4, 4, RHIFormat::RGBA8_UNORM));
    ASSERT_TRUE(lutTexture);
    pass.SetLUT(lutTexture.Get());

    EXPECT_TRUE(pass.IsUsingLUT());
    EXPECT_FALSE(pass.IsSupported());
    EXPECT_FALSE(pass.IsEnabled());
    EXPECT_NE(pass.GetUnsupportedReason().find("LUT"), std::string::npos);

    RenderGraph graph;
    graph.SetDevice(&device);
    RHITextureRef inputTexture =
        device.CreateTexture(RHITextureDesc::RenderTarget(16, 16, RHIFormat::RGBA8_UNORM));
    RHITextureRef outputTexture =
        device.CreateTexture(RHITextureDesc::RenderTarget(16, 16, RHIFormat::RGBA8_UNORM));
    ASSERT_TRUE(inputTexture);
    ASSERT_TRUE(outputTexture);

    RGTextureHandle input = graph.ImportTexture(inputTexture.Get(), RHIResourceState::ShaderResource);
    RGTextureHandle output = graph.ImportTexture(outputTexture.Get(), RHIResourceState::RenderTarget);
    graph.SetExportState(output, RHIResourceState::RenderTarget);

    pass.AddToGraph(graph, input, output);
    graph.Compile();
    EXPECT_TRUE(graph.GetCompileStats().compileValid);
    EXPECT_EQ(graph.GetCompileStats().totalPasses, 0u);
}

TEST_F(RenderPassValidationFixture, ColorGradingNeutralSettingsStillWritesFullscreenPass)
{
    ASSERT_NO_FATAL_FAILURE(Initialize());

    ColorGradingPass pass;
    PostProcessSettings settings;
    settings.enableColorGrading = true;
    settings.contrast = 1.0f;
    settings.saturation = 1.0f;
    settings.brightness = 0.0f;
    pass.Configure(settings);
    pass.SetResources(&pipelineCache, &viewCache);

    ASSERT_TRUE(pass.IsSupported()) << pass.GetUnsupportedReason();
    ASSERT_TRUE(pass.IsEnabled());

    RenderGraph graph;
    graph.SetDevice(&device);

    RHITextureRef inputTexture =
        device.CreateTexture(RHITextureDesc::RenderTarget(64, 64, RHIFormat::RGBA8_UNORM));
    RHITextureRef outputTexture =
        device.CreateTexture(RHITextureDesc::RenderTarget(64, 64, RHIFormat::RGBA8_UNORM));
    ASSERT_TRUE(inputTexture);
    ASSERT_TRUE(outputTexture);

    RGTextureHandle input = graph.ImportTexture(inputTexture.Get(), RHIResourceState::ShaderResource);
    RGTextureHandle output = graph.ImportTexture(outputTexture.Get(), RHIResourceState::RenderTarget);
    graph.SetExportState(output, RHIResourceState::RenderTarget);

    pass.AddToGraph(graph, input, output);
    graph.Compile();

    const auto& stats = graph.GetCompileStats();
    EXPECT_TRUE(stats.compileValid);
    EXPECT_EQ(stats.totalPasses, 1u);
    EXPECT_EQ(stats.culledPasses, 0u);
    EXPECT_EQ(stats.emptyPassUsageCount, 0u);

    RecordingCommandContext ctx;
    graph.Execute(ctx);

    EXPECT_EQ(ctx.beginRenderPassCount, 1u);
    EXPECT_EQ(ctx.endRenderPassCount, 1u);
    ASSERT_EQ(ctx.pipelineSequence.size(), static_cast<size_t>(1));
    EXPECT_EQ(ctx.pipelineSequence[0], pipelineCache.GetColorGradingPipeline(RHIFormat::RGBA8_UNORM));
    ASSERT_EQ(ctx.descriptorSetSequence.size(), static_cast<size_t>(1));
    EXPECT_EQ(ctx.descriptorSetSequence[0], 0u);
    EXPECT_EQ(ctx.drawCount, 1u);
    EXPECT_EQ(ctx.lastDrawVertexCount, 3u);
    EXPECT_EQ(ctx.drawIndexedCount, 0u);

    ASSERT_EQ(ctx.renderPasses.size(), static_cast<size_t>(1));
    EXPECT_EQ(ctx.renderPasses[0].colorAttachmentCount, 1u);
    EXPECT_FALSE(ctx.renderPasses[0].hasDepthStencil);
    EXPECT_EQ(ctx.renderPasses[0].renderArea.width, 64u);
    EXPECT_EQ(ctx.renderPasses[0].renderArea.height, 64u);

    auto descriptorIt = std::find_if(
        device.createdDescriptorSetDescs.begin(),
        device.createdDescriptorSetDescs.end(),
        [](const RHIDescriptorSetDesc& desc)
        {
            return desc.debugName && std::string(desc.debugName) == "ColorGradingDescriptorSet";
        });
    ASSERT_NE(descriptorIt, device.createdDescriptorSetDescs.end());
    EXPECT_EQ(descriptorIt->layout, pipelineCache.GetPostProcessSetLayout());
    ASSERT_EQ(descriptorIt->bindings.size(), static_cast<size_t>(3));
}

TEST_F(RenderPassValidationFixture, ColorGradingUploadsConstantsWithHLSLPacking)
{
    ASSERT_NO_FATAL_FAILURE(Initialize());

    ColorGradingPass pass;
    PostProcessSettings settings;
    settings.enableColorGrading = true;
    pass.Configure(settings);

    ColorGradingConfig config;
    config.mode = ColorGradingMode::LDR;
    config.temperature = 11.0f;
    config.tint = -7.0f;
    config.exposure = 0.25f;
    config.contrast = 1.35f;
    config.saturation = 1.45f;
    config.hueShift = 17.5f;
    config.brightness = 0.27f;
    config.lift = Vec4(1.1f, 1.2f, 1.3f, 0.1f);
    config.gamma = Vec4(0.9f, 0.8f, 0.7f, 0.2f);
    config.gain = Vec4(1.4f, 1.5f, 1.6f, 0.3f);
    config.redChannel = Vec3(0.8f, 0.1f, 0.2f);
    config.greenChannel = Vec3(0.3f, 0.9f, 0.4f);
    config.blueChannel = Vec3(0.5f, 0.6f, 1.0f);
    config.shadowsTint = Vec3(0.2f, 0.3f, 0.4f);
    config.highlightsTint = Vec3(0.7f, 0.8f, 0.9f);
    config.splitToningBalance = -0.25f;
    pass.SetConfig(config);
    pass.SetResources(&pipelineCache, &viewCache);

    ASSERT_TRUE(pass.IsSupported()) << pass.GetUnsupportedReason();
    ASSERT_TRUE(pass.IsEnabled());

    RenderGraph graph;
    graph.SetDevice(&device);

    RHITextureRef inputTexture =
        device.CreateTexture(RHITextureDesc::RenderTarget(32, 24, RHIFormat::RGBA8_UNORM));
    RHITextureRef outputTexture =
        device.CreateTexture(RHITextureDesc::RenderTarget(32, 24, RHIFormat::RGBA8_UNORM));
    ASSERT_TRUE(inputTexture);
    ASSERT_TRUE(outputTexture);

    RGTextureHandle input = graph.ImportTexture(inputTexture.Get(), RHIResourceState::ShaderResource);
    RGTextureHandle output = graph.ImportTexture(outputTexture.Get(), RHIResourceState::RenderTarget);
    graph.SetExportState(output, RHIResourceState::RenderTarget);

    pass.AddToGraph(graph, input, output);
    graph.Compile();

    RecordingCommandContext ctx;
    graph.Execute(ctx);

    ASSERT_EQ(ctx.drawCount, 1u);

    const FakeBuffer* constants = FindCreatedBuffer(device, "ColorGradingConstants");
    ASSERT_NE(constants, nullptr);
    const std::vector<uint8>& storage = constants->GetStorage();
    ASSERT_GE(storage.size(), static_cast<size_t>(176));

    auto readFloat = [&storage](size_t byteOffset)
    {
        float value = 0.0f;
        std::memcpy(&value, storage.data() + byteOffset, sizeof(float));
        return value;
    };

    EXPECT_FLOAT_EQ(readFloat(0), 32.0f);
    EXPECT_FLOAT_EQ(readFloat(4), 24.0f);
    EXPECT_FLOAT_EQ(readFloat(8), 1.0f / 32.0f);
    EXPECT_FLOAT_EQ(readFloat(12), 1.0f / 24.0f);
    EXPECT_FLOAT_EQ(readFloat(16), config.temperature);
    EXPECT_FLOAT_EQ(readFloat(20), config.tint);
    EXPECT_FLOAT_EQ(readFloat(24), config.exposure);
    EXPECT_FLOAT_EQ(readFloat(28), config.contrast);
    EXPECT_FLOAT_EQ(readFloat(32), config.saturation);
    EXPECT_FLOAT_EQ(readFloat(36), config.hueShift);
    EXPECT_FLOAT_EQ(readFloat(40), 0.0f);
    EXPECT_FLOAT_EQ(readFloat(44), 0.0f);

    EXPECT_FLOAT_EQ(readFloat(48), config.lift.x);
    EXPECT_FLOAT_EQ(readFloat(52), config.lift.y);
    EXPECT_FLOAT_EQ(readFloat(56), config.lift.z);
    EXPECT_FLOAT_EQ(readFloat(60), config.lift.w);
    EXPECT_FLOAT_EQ(readFloat(64), config.gamma.x);
    EXPECT_FLOAT_EQ(readFloat(68), config.gamma.y);
    EXPECT_FLOAT_EQ(readFloat(72), config.gamma.z);
    EXPECT_FLOAT_EQ(readFloat(76), config.gamma.w);
    EXPECT_FLOAT_EQ(readFloat(80), config.gain.x);
    EXPECT_FLOAT_EQ(readFloat(84), config.gain.y);
    EXPECT_FLOAT_EQ(readFloat(88), config.gain.z);
    EXPECT_FLOAT_EQ(readFloat(92), config.gain.w);

    EXPECT_FLOAT_EQ(readFloat(96), config.redChannel.x);
    EXPECT_FLOAT_EQ(readFloat(100), config.redChannel.y);
    EXPECT_FLOAT_EQ(readFloat(104), config.redChannel.z);
    EXPECT_FLOAT_EQ(readFloat(108), 0.0f);
    EXPECT_FLOAT_EQ(readFloat(112), config.greenChannel.x);
    EXPECT_FLOAT_EQ(readFloat(116), config.greenChannel.y);
    EXPECT_FLOAT_EQ(readFloat(120), config.greenChannel.z);
    EXPECT_FLOAT_EQ(readFloat(124), 0.0f);
    EXPECT_FLOAT_EQ(readFloat(128), config.blueChannel.x);
    EXPECT_FLOAT_EQ(readFloat(132), config.blueChannel.y);
    EXPECT_FLOAT_EQ(readFloat(136), config.blueChannel.z);
    EXPECT_FLOAT_EQ(readFloat(140), 0.0f);

    EXPECT_FLOAT_EQ(readFloat(144), config.shadowsTint.x);
    EXPECT_FLOAT_EQ(readFloat(148), config.shadowsTint.y);
    EXPECT_FLOAT_EQ(readFloat(152), config.shadowsTint.z);
    EXPECT_FLOAT_EQ(readFloat(156), config.splitToningBalance);
    EXPECT_FLOAT_EQ(readFloat(160), config.highlightsTint.x);
    EXPECT_FLOAT_EQ(readFloat(164), config.highlightsTint.y);
    EXPECT_FLOAT_EQ(readFloat(168), config.highlightsTint.z);
    EXPECT_FLOAT_EQ(readFloat(172), config.brightness);
}

TEST_F(RenderPassValidationFixture, ColorGradingSkipsDrawWhenConstantsCannotMap)
{
    ASSERT_NO_FATAL_FAILURE(Initialize(false));

    ColorGradingPass pass;
    PostProcessSettings settings;
    settings.enableColorGrading = true;
    pass.Configure(settings);
    pass.SetResources(&pipelineCache, &viewCache);

    ASSERT_TRUE(pass.IsSupported()) << pass.GetUnsupportedReason();

    RenderGraph graph;
    graph.SetDevice(&device);

    RHITextureRef inputTexture =
        device.CreateTexture(RHITextureDesc::RenderTarget(16, 16, RHIFormat::RGBA8_UNORM));
    RHITextureRef outputTexture =
        device.CreateTexture(RHITextureDesc::RenderTarget(16, 16, RHIFormat::RGBA8_UNORM));
    ASSERT_TRUE(inputTexture);
    ASSERT_TRUE(outputTexture);

    RGTextureHandle input = graph.ImportTexture(inputTexture.Get(), RHIResourceState::ShaderResource);
    RGTextureHandle output = graph.ImportTexture(outputTexture.Get(), RHIResourceState::RenderTarget);
    graph.SetExportState(output, RHIResourceState::RenderTarget);

    pass.AddToGraph(graph, input, output);
    graph.Compile();

    RecordingCommandContext ctx;
    graph.Execute(ctx);

    EXPECT_EQ(ctx.drawCount, 0u);
    EXPECT_EQ(ctx.beginRenderPassCount, 0u);
}

TEST_F(RenderPassValidationFixture, ChromaticAberrationRequiresResourcesBeforeReportingSupported)
{
    ChromaticAberrationPass pass;
    PostProcessSettings settings;
    settings.enableChromaticAberration = true;
    pass.Configure(settings);

    EXPECT_TRUE(pass.IsRequestedEnabled());
    EXPECT_FALSE(pass.IsSupported());
    EXPECT_FALSE(pass.IsEnabled());
    EXPECT_FALSE(pass.GetUnsupportedReason().empty());
}

TEST_F(RenderPassValidationFixture, ChromaticAberrationSupportedWithResources)
{
    ASSERT_NO_FATAL_FAILURE(Initialize());

    ChromaticAberrationPass pass;
    PostProcessSettings settings;
    settings.enableChromaticAberration = true;
    pass.Configure(settings);
    pass.SetResources(&pipelineCache, &viewCache);

    EXPECT_TRUE(pass.IsRequestedEnabled());
    EXPECT_TRUE(pass.IsSupported()) << pass.GetUnsupportedReason();
    EXPECT_TRUE(pass.IsEnabled());
}

TEST_F(RenderPassValidationFixture, ChromaticAberrationRejectsSpectralBeforeGraphScheduling)
{
    ASSERT_NO_FATAL_FAILURE(Initialize());

    ChromaticAberrationPass pass;
    PostProcessSettings settings;
    settings.enableChromaticAberration = true;
    pass.Configure(settings);
    pass.SetResources(&pipelineCache, &viewCache);
    pass.SetSpectralSampling(true);

    EXPECT_TRUE(pass.IsRequestedEnabled());
    EXPECT_FALSE(pass.IsSupported());
    EXPECT_FALSE(pass.IsEnabled());
    EXPECT_NE(pass.GetUnsupportedReason().find("spectral"), std::string::npos);

    RenderGraph graph;
    graph.SetDevice(&device);

    RHITextureRef inputTexture =
        device.CreateTexture(RHITextureDesc::RenderTarget(16, 16, RHIFormat::RGBA8_UNORM));
    RHITextureRef outputTexture =
        device.CreateTexture(RHITextureDesc::RenderTarget(16, 16, RHIFormat::RGBA8_UNORM));
    ASSERT_TRUE(inputTexture);
    ASSERT_TRUE(outputTexture);

    RGTextureHandle input = graph.ImportTexture(inputTexture.Get(), RHIResourceState::ShaderResource);
    RGTextureHandle output = graph.ImportTexture(outputTexture.Get(), RHIResourceState::RenderTarget);
    graph.SetExportState(output, RHIResourceState::RenderTarget);

    pass.AddToGraph(graph, input, output);
    graph.Compile();

    EXPECT_TRUE(graph.GetCompileStats().compileValid);
    EXPECT_EQ(graph.GetCompileStats().totalPasses, 0u);
}

TEST_F(RenderPassValidationFixture, ChromaticAberrationZeroIntensityStillWritesFullscreenPass)
{
    ASSERT_NO_FATAL_FAILURE(Initialize());

    ChromaticAberrationPass pass;
    PostProcessSettings settings;
    settings.enableChromaticAberration = true;
    settings.chromaticAberrationIntensity = 0.0f;
    pass.Configure(settings);
    pass.SetResources(&pipelineCache, &viewCache);

    ASSERT_TRUE(pass.IsSupported()) << pass.GetUnsupportedReason();
    ASSERT_TRUE(pass.IsEnabled());

    RenderGraph graph;
    graph.SetDevice(&device);

    RHITextureRef inputTexture =
        device.CreateTexture(RHITextureDesc::RenderTarget(64, 64, RHIFormat::RGBA8_UNORM));
    RHITextureRef outputTexture =
        device.CreateTexture(RHITextureDesc::RenderTarget(64, 64, RHIFormat::RGBA8_UNORM));
    ASSERT_TRUE(inputTexture);
    ASSERT_TRUE(outputTexture);

    RGTextureHandle input = graph.ImportTexture(inputTexture.Get(), RHIResourceState::ShaderResource);
    RGTextureHandle output = graph.ImportTexture(outputTexture.Get(), RHIResourceState::RenderTarget);
    graph.SetExportState(output, RHIResourceState::RenderTarget);

    pass.AddToGraph(graph, input, output);
    graph.Compile();

    const auto& stats = graph.GetCompileStats();
    EXPECT_TRUE(stats.compileValid);
    EXPECT_EQ(stats.totalPasses, 1u);
    EXPECT_EQ(stats.culledPasses, 0u);

    RecordingCommandContext ctx;
    graph.Execute(ctx);

    EXPECT_EQ(ctx.beginRenderPassCount, 1u);
    EXPECT_EQ(ctx.endRenderPassCount, 1u);
    ASSERT_EQ(ctx.pipelineSequence.size(), static_cast<size_t>(1));
    EXPECT_EQ(ctx.pipelineSequence[0], pipelineCache.GetChromaticAberrationPipeline(RHIFormat::RGBA8_UNORM));
    EXPECT_EQ(ctx.drawCount, 1u);
    EXPECT_EQ(ctx.lastDrawVertexCount, 3u);

    auto descriptorIt = std::find_if(
        device.createdDescriptorSetDescs.begin(),
        device.createdDescriptorSetDescs.end(),
        [](const RHIDescriptorSetDesc& desc)
        {
            return desc.debugName && std::string(desc.debugName) == "ChromaticAberrationDescriptorSet";
        });
    ASSERT_NE(descriptorIt, device.createdDescriptorSetDescs.end());
    EXPECT_EQ(descriptorIt->layout, pipelineCache.GetPostProcessSetLayout());
    ASSERT_EQ(descriptorIt->bindings.size(), static_cast<size_t>(3));
}

TEST_F(RenderPassValidationFixture, ChromaticAberrationUploadsConstantsWithHLSLPacking)
{
    ASSERT_NO_FATAL_FAILURE(Initialize());

    ChromaticAberrationPass pass;
    PostProcessSettings settings;
    settings.enableChromaticAberration = true;
    pass.Configure(settings);

    ChromaticAberrationConfig config;
    config.intensity = 0.42f;
    config.startOffset = 0.15f;
    config.radialFalloff = false;
    config.useSpectral = false;
    config.redOffset = Vec2(-1.25f, 0.5f);
    config.greenOffset = Vec2(0.25f, -0.25f);
    config.blueOffset = Vec2(1.5f, 0.75f);
    pass.SetConfig(config);
    pass.SetResources(&pipelineCache, &viewCache);

    ASSERT_TRUE(pass.IsSupported()) << pass.GetUnsupportedReason();
    ASSERT_TRUE(pass.IsEnabled());

    RenderGraph graph;
    graph.SetDevice(&device);

    RHITextureRef inputTexture =
        device.CreateTexture(RHITextureDesc::RenderTarget(40, 20, RHIFormat::RGBA8_UNORM));
    RHITextureRef outputTexture =
        device.CreateTexture(RHITextureDesc::RenderTarget(40, 20, RHIFormat::RGBA8_UNORM));
    ASSERT_TRUE(inputTexture);
    ASSERT_TRUE(outputTexture);

    RGTextureHandle input = graph.ImportTexture(inputTexture.Get(), RHIResourceState::ShaderResource);
    RGTextureHandle output = graph.ImportTexture(outputTexture.Get(), RHIResourceState::RenderTarget);
    graph.SetExportState(output, RHIResourceState::RenderTarget);

    pass.AddToGraph(graph, input, output);
    graph.Compile();

    RecordingCommandContext ctx;
    graph.Execute(ctx);

    ASSERT_EQ(ctx.drawCount, 1u);

    const FakeBuffer* constants = FindCreatedBuffer(device, "ChromaticAberrationConstants");
    ASSERT_NE(constants, nullptr);
    const std::vector<uint8>& storage = constants->GetStorage();
    ASSERT_GE(storage.size(), static_cast<size_t>(64));

    auto readFloat = [&storage](size_t byteOffset)
    {
        float value = 0.0f;
        std::memcpy(&value, storage.data() + byteOffset, sizeof(float));
        return value;
    };

    EXPECT_FLOAT_EQ(readFloat(0), 40.0f);
    EXPECT_FLOAT_EQ(readFloat(4), 20.0f);
    EXPECT_FLOAT_EQ(readFloat(8), 1.0f / 40.0f);
    EXPECT_FLOAT_EQ(readFloat(12), 1.0f / 20.0f);
    EXPECT_FLOAT_EQ(readFloat(16), config.intensity);
    EXPECT_FLOAT_EQ(readFloat(20), config.startOffset);
    EXPECT_FLOAT_EQ(readFloat(24), 0.0f);
    EXPECT_FLOAT_EQ(readFloat(28), 0.0f);
    EXPECT_FLOAT_EQ(readFloat(32), config.redOffset.x);
    EXPECT_FLOAT_EQ(readFloat(36), config.redOffset.y);
    EXPECT_FLOAT_EQ(readFloat(40), config.greenOffset.x);
    EXPECT_FLOAT_EQ(readFloat(44), config.greenOffset.y);
    EXPECT_FLOAT_EQ(readFloat(48), config.blueOffset.x);
    EXPECT_FLOAT_EQ(readFloat(52), config.blueOffset.y);
    EXPECT_FLOAT_EQ(readFloat(56), 0.0f);
    EXPECT_FLOAT_EQ(readFloat(60), 0.0f);
}

TEST_F(RenderPassValidationFixture, ChromaticAberrationSkipsDrawWhenConstantsCannotMap)
{
    ASSERT_NO_FATAL_FAILURE(Initialize(false));

    ChromaticAberrationPass pass;
    PostProcessSettings settings;
    settings.enableChromaticAberration = true;
    pass.Configure(settings);
    pass.SetResources(&pipelineCache, &viewCache);

    ASSERT_TRUE(pass.IsSupported()) << pass.GetUnsupportedReason();

    RenderGraph graph;
    graph.SetDevice(&device);

    RHITextureRef inputTexture =
        device.CreateTexture(RHITextureDesc::RenderTarget(16, 16, RHIFormat::RGBA8_UNORM));
    RHITextureRef outputTexture =
        device.CreateTexture(RHITextureDesc::RenderTarget(16, 16, RHIFormat::RGBA8_UNORM));
    ASSERT_TRUE(inputTexture);
    ASSERT_TRUE(outputTexture);

    RGTextureHandle input = graph.ImportTexture(inputTexture.Get(), RHIResourceState::ShaderResource);
    RGTextureHandle output = graph.ImportTexture(outputTexture.Get(), RHIResourceState::RenderTarget);
    graph.SetExportState(output, RHIResourceState::RenderTarget);

    pass.AddToGraph(graph, input, output);
    graph.Compile();

    RecordingCommandContext ctx;
    graph.Execute(ctx);

    EXPECT_EQ(ctx.drawCount, 0u);
    EXPECT_EQ(ctx.beginRenderPassCount, 0u);
}

TEST_F(RenderPassValidationFixture, VignetteRequiresResourcesBeforeReportingSupported)
{
    VignettePass pass;
    PostProcessSettings settings;
    settings.enableVignette = true;
    pass.Configure(settings);

    EXPECT_TRUE(pass.IsRequestedEnabled());
    EXPECT_FALSE(pass.IsSupported());
    EXPECT_FALSE(pass.IsEnabled());
    EXPECT_FALSE(pass.GetUnsupportedReason().empty());
}

TEST_F(RenderPassValidationFixture, VignetteAddsLiveGraphPassAndDrawsFullscreenTriangle)
{
    ASSERT_NO_FATAL_FAILURE(Initialize());

    VignettePass pass;
    PostProcessSettings settings;
    settings.enableVignette = true;
    settings.vignetteIntensity = 0.4f;
    settings.vignetteRadius = 0.8f;
    pass.Configure(settings);
    pass.SetResources(&pipelineCache, &viewCache);

    ASSERT_TRUE(pass.IsSupported()) << pass.GetUnsupportedReason();
    ASSERT_TRUE(pass.IsEnabled());

    RenderGraph graph;
    graph.SetDevice(&device);

    RHITextureRef inputTexture =
        device.CreateTexture(RHITextureDesc::RenderTarget(64, 64, RHIFormat::RGBA8_UNORM));
    RHITextureRef outputTexture =
        device.CreateTexture(RHITextureDesc::RenderTarget(64, 64, RHIFormat::RGBA8_UNORM));
    ASSERT_TRUE(inputTexture);
    ASSERT_TRUE(outputTexture);

    RGTextureHandle input = graph.ImportTexture(inputTexture.Get(), RHIResourceState::ShaderResource);
    RGTextureHandle output = graph.ImportTexture(outputTexture.Get(), RHIResourceState::RenderTarget);
    graph.SetExportState(output, RHIResourceState::RenderTarget);

    pass.AddToGraph(graph, input, output);
    graph.Compile();

    const auto& stats = graph.GetCompileStats();
    EXPECT_TRUE(stats.compileValid);
    EXPECT_EQ(stats.totalPasses, 1u);
    EXPECT_EQ(stats.culledPasses, 0u);
    EXPECT_EQ(stats.emptyPassUsageCount, 0u);

    RecordingCommandContext ctx;
    graph.Execute(ctx);

    EXPECT_EQ(ctx.beginRenderPassCount, 1u);
    EXPECT_EQ(ctx.endRenderPassCount, 1u);
    ASSERT_EQ(ctx.pipelineSequence.size(), static_cast<size_t>(1));
    EXPECT_EQ(ctx.pipelineSequence[0], pipelineCache.GetVignettePipeline(RHIFormat::RGBA8_UNORM));
    ASSERT_EQ(ctx.descriptorSetSequence.size(), static_cast<size_t>(1));
    EXPECT_EQ(ctx.descriptorSetSequence[0], 0u);
    EXPECT_EQ(ctx.drawCount, 1u);
    EXPECT_EQ(ctx.lastDrawVertexCount, 3u);
    EXPECT_EQ(ctx.drawIndexedCount, 0u);

    ASSERT_EQ(ctx.renderPasses.size(), static_cast<size_t>(1));
    EXPECT_EQ(ctx.renderPasses[0].colorAttachmentCount, 1u);
    EXPECT_FALSE(ctx.renderPasses[0].hasDepthStencil);
    EXPECT_EQ(ctx.renderPasses[0].renderArea.width, 64u);
    EXPECT_EQ(ctx.renderPasses[0].renderArea.height, 64u);

    auto descriptorIt = std::find_if(
        device.createdDescriptorSetDescs.begin(),
        device.createdDescriptorSetDescs.end(),
        [](const RHIDescriptorSetDesc& desc)
        {
            return desc.debugName && std::string(desc.debugName) == "VignetteDescriptorSet";
        });
    ASSERT_NE(descriptorIt, device.createdDescriptorSetDescs.end());
    EXPECT_EQ(descriptorIt->layout, pipelineCache.GetPostProcessSetLayout());
    ASSERT_EQ(descriptorIt->bindings.size(), static_cast<size_t>(3));

    const auto hasBinding = [descriptorIt](uint32 binding,
                                           bool expectBuffer,
                                           bool expectTexture,
                                           bool expectSampler)
    {
        auto it = std::find_if(descriptorIt->bindings.begin(),
                               descriptorIt->bindings.end(),
                               [binding](const RHIDescriptorBinding& descriptorBinding)
                               {
                                   return descriptorBinding.binding == binding;
                               });
        if (it == descriptorIt->bindings.end())
            return false;

        return (it->buffer != nullptr) == expectBuffer &&
               (it->textureView != nullptr) == expectTexture &&
               (it->sampler != nullptr) == expectSampler;
    };

    EXPECT_TRUE(hasBinding(0, true, false, false));
    EXPECT_TRUE(hasBinding(1, false, true, false));
    EXPECT_TRUE(hasBinding(2, false, false, true));
}

TEST_F(RenderPassValidationFixture, VignetteSkipsDrawWhenConstantsCannotMap)
{
    ASSERT_NO_FATAL_FAILURE(Initialize(false));

    VignettePass pass;
    PostProcessSettings settings;
    settings.enableVignette = true;
    pass.Configure(settings);
    pass.SetResources(&pipelineCache, &viewCache);

    ASSERT_TRUE(pass.IsSupported()) << pass.GetUnsupportedReason();

    RenderGraph graph;
    graph.SetDevice(&device);

    RHITextureRef inputTexture =
        device.CreateTexture(RHITextureDesc::RenderTarget(16, 16, RHIFormat::RGBA8_UNORM));
    RHITextureRef outputTexture =
        device.CreateTexture(RHITextureDesc::RenderTarget(16, 16, RHIFormat::RGBA8_UNORM));
    ASSERT_TRUE(inputTexture);
    ASSERT_TRUE(outputTexture);

    RGTextureHandle input = graph.ImportTexture(inputTexture.Get(), RHIResourceState::ShaderResource);
    RGTextureHandle output = graph.ImportTexture(outputTexture.Get(), RHIResourceState::RenderTarget);
    graph.SetExportState(output, RHIResourceState::RenderTarget);

    pass.AddToGraph(graph, input, output);
    graph.Compile();

    RecordingCommandContext ctx;
    graph.Execute(ctx);

    EXPECT_EQ(ctx.drawCount, 0u);
    EXPECT_EQ(ctx.beginRenderPassCount, 0u);
}

TEST_F(RenderPassValidationFixture, PostProcessStackRunsBloomBeforeToneMappingThroughIntermediate)
{
    ASSERT_NO_FATAL_FAILURE(Initialize());

    RenderGraph graph;
    graph.SetDevice(&device);

    RHITextureRef inputTexture =
        device.CreateTexture(RHITextureDesc::RenderTarget(32, 32, RHIFormat::RGBA16_FLOAT));
    RHITextureRef outputTexture =
        device.CreateTexture(RHITextureDesc::RenderTarget(32, 32, RHIFormat::RGBA8_UNORM));
    ASSERT_TRUE(inputTexture);
    ASSERT_TRUE(outputTexture);

    RGTextureHandle input = graph.ImportTexture(inputTexture.Get(), RHIResourceState::ShaderResource);
    RGTextureHandle output = graph.ImportTexture(outputTexture.Get(), RHIResourceState::RenderTarget);
    graph.SetExportState(output, RHIResourceState::RenderTarget);

    PostProcessSettings settings;
    settings.enableBloom = true;
    settings.enableToneMapping = true;

    PostProcessStack stack;
    auto* bloom = stack.AddEffect<BloomPass>();
    auto* toneMapping = stack.AddEffect<ToneMappingPass>();
    bloom->Configure(settings);
    toneMapping->Configure(settings);
    bloom->SetResources(&pipelineCache, &viewCache);
    toneMapping->SetResources(&pipelineCache, &viewCache);

    stack.Execute(graph, input, output);

    const PostProcessStackExecuteStats& executeStats = stack.GetLastExecuteStats();
    EXPECT_FALSE(executeStats.noEffectNoWork);
    EXPECT_EQ(executeStats.enabledEffectCount, 2u);
    EXPECT_EQ(executeStats.graphPassCount, 2u);
    EXPECT_EQ(executeStats.transientIntermediateCount, 1u);
    EXPECT_EQ(executeStats.transientIntermediateFormat, RHIFormat::RGBA16_FLOAT);
    EXPECT_EQ(executeStats.finalOutputFormat, RHIFormat::RGBA8_UNORM);
    EXPECT_TRUE(executeStats.toneMappingBoundaryValid);
    EXPECT_TRUE(executeStats.toneMappingBoundaryWarning.empty());

    graph.Compile();
    const auto& graphStats = graph.GetCompileStats();
    EXPECT_TRUE(graphStats.compileValid);
    EXPECT_EQ(graphStats.totalPasses, 2u);

    RecordingCommandContext ctx;
    graph.Execute(ctx);

    ASSERT_EQ(ctx.pipelineSequence.size(), static_cast<size_t>(2));
    EXPECT_EQ(ctx.pipelineSequence[0], pipelineCache.GetBloomPipeline(RHIFormat::RGBA16_FLOAT));
    EXPECT_EQ(ctx.pipelineSequence[1], pipelineCache.GetToneMappingPipeline());
    EXPECT_EQ(ctx.drawCount, 2u);
    EXPECT_EQ(ctx.lastDrawVertexCount, 3u);
    EXPECT_EQ(ctx.beginRenderPassCount, 2u);
    EXPECT_EQ(ctx.endRenderPassCount, 2u);
}

TEST_F(RenderPassValidationFixture, PostProcessStackRunsBloomToneMappingColorGradingChromaticVignetteFXAAWithHDRAndLDRIntermediates)
{
    ASSERT_NO_FATAL_FAILURE(Initialize());

    RenderGraph graph;
    graph.SetDevice(&device);

    RHITextureRef inputTexture =
        device.CreateTexture(RHITextureDesc::RenderTarget(32, 32, RHIFormat::RGBA16_FLOAT));
    RHITextureRef outputTexture =
        device.CreateTexture(RHITextureDesc::RenderTarget(32, 32, RHIFormat::RGBA8_UNORM));
    ASSERT_TRUE(inputTexture);
    ASSERT_TRUE(outputTexture);

    RGTextureHandle input = graph.ImportTexture(inputTexture.Get(), RHIResourceState::ShaderResource);
    RGTextureHandle output = graph.ImportTexture(outputTexture.Get(), RHIResourceState::RenderTarget);
    graph.SetExportState(output, RHIResourceState::RenderTarget);

    PostProcessSettings settings;
    settings.enableBloom = true;
    settings.enableToneMapping = true;
    settings.enableColorGrading = true;
    settings.enableChromaticAberration = true;
    settings.enableVignette = true;
    settings.enableFXAA = true;

    PostProcessStack stack;
    auto* bloom = stack.AddEffect<BloomPass>();
    auto* toneMapping = stack.AddEffect<ToneMappingPass>();
    auto* colorGrading = stack.AddEffect<ColorGradingPass>();
    auto* chromaticAberration = stack.AddEffect<ChromaticAberrationPass>();
    auto* vignette = stack.AddEffect<VignettePass>();
    auto* fxaa = stack.AddEffect<FXAAPass>();
    bloom->Configure(settings);
    toneMapping->Configure(settings);
    colorGrading->Configure(settings);
    chromaticAberration->Configure(settings);
    vignette->Configure(settings);
    fxaa->Configure(settings);
    bloom->SetResources(&pipelineCache, &viewCache);
    toneMapping->SetResources(&pipelineCache, &viewCache);
    colorGrading->SetResources(&pipelineCache, &viewCache);
    chromaticAberration->SetResources(&pipelineCache, &viewCache);
    vignette->SetResources(&pipelineCache, &viewCache);
    fxaa->SetResources(&pipelineCache, &viewCache);

    stack.Execute(graph, input, output);

    const PostProcessStackExecuteStats& executeStats = stack.GetLastExecuteStats();
    EXPECT_FALSE(executeStats.noEffectNoWork);
    EXPECT_EQ(executeStats.enabledEffectCount, 6u);
    EXPECT_EQ(executeStats.graphPassCount, 6u);
    EXPECT_EQ(executeStats.transientIntermediateCount, 5u);
    EXPECT_EQ(executeStats.hdrIntermediateCount, 1u);
    EXPECT_EQ(executeStats.hdrIntermediateFormat, RHIFormat::RGBA16_FLOAT);
    EXPECT_EQ(executeStats.ldrIntermediateCount, 4u);
    EXPECT_EQ(executeStats.ldrIntermediateFormat, RHIFormat::RGBA8_UNORM);
    EXPECT_EQ(executeStats.transientIntermediateFormat, RHIFormat::RGBA8_UNORM);
    EXPECT_EQ(executeStats.finalOutputFormat, RHIFormat::RGBA8_UNORM);
    EXPECT_TRUE(executeStats.toneMappingBoundaryValid);
    EXPECT_TRUE(executeStats.toneMappingBoundaryWarning.empty());

    graph.Compile();
    const auto& graphStats = graph.GetCompileStats();
    EXPECT_TRUE(graphStats.compileValid);
    EXPECT_EQ(graphStats.totalPasses, 6u);

    RecordingCommandContext ctx;
    graph.Execute(ctx);

    ASSERT_EQ(ctx.pipelineSequence.size(), static_cast<size_t>(6));
    EXPECT_EQ(ctx.pipelineSequence[0], pipelineCache.GetBloomPipeline(RHIFormat::RGBA16_FLOAT));
    EXPECT_EQ(ctx.pipelineSequence[1], pipelineCache.GetToneMappingPipeline());
    EXPECT_EQ(ctx.pipelineSequence[2], pipelineCache.GetColorGradingPipeline(RHIFormat::RGBA8_UNORM));
    EXPECT_EQ(ctx.pipelineSequence[3], pipelineCache.GetChromaticAberrationPipeline(RHIFormat::RGBA8_UNORM));
    EXPECT_EQ(ctx.pipelineSequence[4], pipelineCache.GetVignettePipeline(RHIFormat::RGBA8_UNORM));
    EXPECT_EQ(ctx.pipelineSequence[5], pipelineCache.GetFXAAPipeline(RHIFormat::RGBA8_UNORM));
    EXPECT_EQ(ctx.drawCount, 6u);
    EXPECT_EQ(ctx.lastDrawVertexCount, 3u);
    EXPECT_EQ(ctx.beginRenderPassCount, 6u);
    EXPECT_EQ(ctx.endRenderPassCount, 6u);
}

TEST_F(RenderPassValidationFixture, PostProcessStackKeepsZeroIntensityVignetteAsPassThrough)
{
    ASSERT_NO_FATAL_FAILURE(Initialize());

    RenderGraph graph;
    graph.SetDevice(&device);

    RHITextureRef inputTexture =
        device.CreateTexture(RHITextureDesc::RenderTarget(32, 32, RHIFormat::RGBA16_FLOAT));
    RHITextureRef outputTexture =
        device.CreateTexture(RHITextureDesc::RenderTarget(32, 32, RHIFormat::RGBA8_UNORM));
    ASSERT_TRUE(inputTexture);
    ASSERT_TRUE(outputTexture);

    RGTextureHandle input = graph.ImportTexture(inputTexture.Get(), RHIResourceState::ShaderResource);
    RGTextureHandle output = graph.ImportTexture(outputTexture.Get(), RHIResourceState::RenderTarget);
    graph.SetExportState(output, RHIResourceState::RenderTarget);

    PostProcessSettings settings;
    settings.enableToneMapping = true;
    settings.enableVignette = true;
    settings.vignetteIntensity = 0.0f;
    settings.enableFXAA = true;

    PostProcessStack stack;
    auto* toneMapping = stack.AddEffect<ToneMappingPass>();
    auto* vignette = stack.AddEffect<VignettePass>();
    auto* fxaa = stack.AddEffect<FXAAPass>();
    toneMapping->Configure(settings);
    vignette->Configure(settings);
    fxaa->Configure(settings);
    toneMapping->SetResources(&pipelineCache, &viewCache);
    vignette->SetResources(&pipelineCache, &viewCache);
    fxaa->SetResources(&pipelineCache, &viewCache);

    stack.Execute(graph, input, output);

    const PostProcessStackExecuteStats& executeStats = stack.GetLastExecuteStats();
    EXPECT_FALSE(executeStats.noEffectNoWork);
    EXPECT_EQ(executeStats.enabledEffectCount, 3u);
    EXPECT_EQ(executeStats.graphPassCount, 3u);
    EXPECT_EQ(executeStats.transientIntermediateCount, 2u);
    EXPECT_EQ(executeStats.ldrIntermediateCount, 2u);
    EXPECT_TRUE(executeStats.toneMappingBoundaryValid);

    graph.Compile();
    const auto& graphStats = graph.GetCompileStats();
    EXPECT_TRUE(graphStats.compileValid);
    EXPECT_EQ(graphStats.totalPasses, 3u);

    RecordingCommandContext ctx;
    graph.Execute(ctx);

    ASSERT_EQ(ctx.pipelineSequence.size(), static_cast<size_t>(3));
    EXPECT_EQ(ctx.pipelineSequence[0], pipelineCache.GetToneMappingPipeline());
    EXPECT_EQ(ctx.pipelineSequence[1], pipelineCache.GetVignettePipeline(RHIFormat::RGBA8_UNORM));
    EXPECT_EQ(ctx.pipelineSequence[2], pipelineCache.GetFXAAPipeline(RHIFormat::RGBA8_UNORM));
    EXPECT_EQ(ctx.drawCount, 3u);
    EXPECT_EQ(ctx.beginRenderPassCount, 3u);
    EXPECT_EQ(ctx.endRenderPassCount, 3u);
}

TEST_F(RenderPassValidationFixture, PostProcessStackReportsInvalidHDRPassAfterToneMapping)
{
    ASSERT_NO_FATAL_FAILURE(Initialize());

    RenderGraph graph;
    graph.SetDevice(&device);

    RHITextureRef inputTexture =
        device.CreateTexture(RHITextureDesc::RenderTarget(32, 32, RHIFormat::RGBA16_FLOAT));
    RHITextureRef outputTexture =
        device.CreateTexture(RHITextureDesc::RenderTarget(32, 32, RHIFormat::RGBA8_UNORM));
    ASSERT_TRUE(inputTexture);
    ASSERT_TRUE(outputTexture);

    RGTextureHandle input = graph.ImportTexture(inputTexture.Get(), RHIResourceState::ShaderResource);
    RGTextureHandle output = graph.ImportTexture(outputTexture.Get(), RHIResourceState::RenderTarget);

    PostProcessStack stack;
    (void)stack.AddEffect<RecordingPostProcessPass>("ToneMapping", 100);
    (void)stack.AddEffect<RecordingPostProcessPass>("Bloom", 200);

    stack.Execute(graph, input, output);

    const PostProcessStackExecuteStats& executeStats = stack.GetLastExecuteStats();
    EXPECT_FALSE(executeStats.toneMappingBoundaryValid);
    EXPECT_FALSE(executeStats.toneMappingBoundaryWarning.empty());
    EXPECT_EQ(executeStats.graphPassCount, 0u);
    EXPECT_EQ(executeStats.transientIntermediateCount, 0u);
    EXPECT_EQ(executeStats.transientIntermediateFormat, RHIFormat::Unknown);
    EXPECT_EQ(executeStats.ldrIntermediateCount, 0u);
    EXPECT_EQ(executeStats.ldrIntermediateFormat, RHIFormat::Unknown);
    EXPECT_EQ(executeStats.finalOutputFormat, RHIFormat::RGBA8_UNORM);

    graph.Compile();
    const auto& graphStats = graph.GetCompileStats();
    EXPECT_TRUE(graphStats.compileValid);
    EXPECT_EQ(graphStats.totalPasses, 0u);
}

TEST_F(RenderPassValidationFixture, PostProcessStackReportsInvalidHDRPassAfterLDRChain)
{
    ASSERT_NO_FATAL_FAILURE(Initialize());

    RenderGraph graph;
    graph.SetDevice(&device);

    RHITextureRef inputTexture =
        device.CreateTexture(RHITextureDesc::RenderTarget(32, 32, RHIFormat::RGBA16_FLOAT));
    RHITextureRef outputTexture =
        device.CreateTexture(RHITextureDesc::RenderTarget(32, 32, RHIFormat::RGBA8_UNORM));
    ASSERT_TRUE(inputTexture);
    ASSERT_TRUE(outputTexture);

    RGTextureHandle input = graph.ImportTexture(inputTexture.Get(), RHIResourceState::ShaderResource);
    RGTextureHandle output = graph.ImportTexture(outputTexture.Get(), RHIResourceState::RenderTarget);

    PostProcessStack stack;
    (void)stack.AddEffect<RecordingPostProcessPass>("ToneMapping", 100);
    (void)stack.AddEffect<RecordingPostProcessPass>("Vignette", 200);
    (void)stack.AddEffect<RecordingPostProcessPass>("Bloom", 300);

    stack.Execute(graph, input, output);

    const PostProcessStackExecuteStats& executeStats = stack.GetLastExecuteStats();
    EXPECT_FALSE(executeStats.toneMappingBoundaryValid);
    EXPECT_FALSE(executeStats.toneMappingBoundaryWarning.empty());
    EXPECT_EQ(executeStats.graphPassCount, 0u);
    EXPECT_EQ(executeStats.transientIntermediateCount, 0u);
    EXPECT_EQ(executeStats.finalOutputFormat, RHIFormat::RGBA8_UNORM);

    graph.Compile();
    const auto& graphStats = graph.GetCompileStats();
    EXPECT_TRUE(graphStats.compileValid);
    EXPECT_EQ(graphStats.totalPasses, 0u);
}

TEST_F(RenderPassValidationFixture, PostProcessStackReportsInvalidHDRPassAfterColorGradingLDRChain)
{
    ASSERT_NO_FATAL_FAILURE(Initialize());

    RenderGraph graph;
    graph.SetDevice(&device);

    RHITextureRef inputTexture =
        device.CreateTexture(RHITextureDesc::RenderTarget(32, 32, RHIFormat::RGBA16_FLOAT));
    RHITextureRef outputTexture =
        device.CreateTexture(RHITextureDesc::RenderTarget(32, 32, RHIFormat::RGBA8_UNORM));
    ASSERT_TRUE(inputTexture);
    ASSERT_TRUE(outputTexture);

    RGTextureHandle input = graph.ImportTexture(inputTexture.Get(), RHIResourceState::ShaderResource);
    RGTextureHandle output = graph.ImportTexture(outputTexture.Get(), RHIResourceState::RenderTarget);

    PostProcessStack stack;
    (void)stack.AddEffect<RecordingPostProcessPass>("ToneMapping", 100);
    (void)stack.AddEffect<RecordingPostProcessPass>("ColorGrading", 200);
    (void)stack.AddEffect<RecordingPostProcessPass>("Bloom", 300);

    stack.Execute(graph, input, output);

    const PostProcessStackExecuteStats& executeStats = stack.GetLastExecuteStats();
    EXPECT_FALSE(executeStats.toneMappingBoundaryValid);
    EXPECT_FALSE(executeStats.toneMappingBoundaryWarning.empty());
    EXPECT_EQ(executeStats.graphPassCount, 0u);
    EXPECT_EQ(executeStats.transientIntermediateCount, 0u);
    EXPECT_EQ(executeStats.finalOutputFormat, RHIFormat::RGBA8_UNORM);

    graph.Compile();
    const auto& graphStats = graph.GetCompileStats();
    EXPECT_TRUE(graphStats.compileValid);
    EXPECT_EQ(graphStats.totalPasses, 0u);
}

TEST_F(RenderPassValidationFixture, PostProcessStackReportsInvalidLDREffectBeforeToneMapping)
{
    ASSERT_NO_FATAL_FAILURE(Initialize());

    RenderGraph graph;
    graph.SetDevice(&device);

    RHITextureRef inputTexture =
        device.CreateTexture(RHITextureDesc::RenderTarget(32, 32, RHIFormat::RGBA16_FLOAT));
    RHITextureRef outputTexture =
        device.CreateTexture(RHITextureDesc::RenderTarget(32, 32, RHIFormat::RGBA8_UNORM));
    ASSERT_TRUE(inputTexture);
    ASSERT_TRUE(outputTexture);

    RGTextureHandle input = graph.ImportTexture(inputTexture.Get(), RHIResourceState::ShaderResource);
    RGTextureHandle output = graph.ImportTexture(outputTexture.Get(), RHIResourceState::RenderTarget);

    PostProcessStack stack;
    (void)stack.AddEffect<RecordingPostProcessPass>("FXAA", 100);

    stack.Execute(graph, input, output);

    const PostProcessStackExecuteStats& executeStats = stack.GetLastExecuteStats();
    EXPECT_FALSE(executeStats.toneMappingBoundaryValid);
    EXPECT_FALSE(executeStats.toneMappingBoundaryWarning.empty());
    EXPECT_EQ(executeStats.graphPassCount, 0u);
    EXPECT_EQ(executeStats.transientIntermediateCount, 0u);
    EXPECT_EQ(executeStats.finalOutputFormat, RHIFormat::RGBA8_UNORM);

    graph.Compile();
    const auto& graphStats = graph.GetCompileStats();
    EXPECT_TRUE(graphStats.compileValid);
    EXPECT_EQ(graphStats.totalPasses, 0u);
}

TEST_F(RenderPassValidationFixture, PostProcessStackReportsInvalidColorGradingBeforeToneMapping)
{
    ASSERT_NO_FATAL_FAILURE(Initialize());

    RenderGraph graph;
    graph.SetDevice(&device);

    RHITextureRef inputTexture =
        device.CreateTexture(RHITextureDesc::RenderTarget(32, 32, RHIFormat::RGBA16_FLOAT));
    RHITextureRef outputTexture =
        device.CreateTexture(RHITextureDesc::RenderTarget(32, 32, RHIFormat::RGBA8_UNORM));
    ASSERT_TRUE(inputTexture);
    ASSERT_TRUE(outputTexture);

    RGTextureHandle input = graph.ImportTexture(inputTexture.Get(), RHIResourceState::ShaderResource);
    RGTextureHandle output = graph.ImportTexture(outputTexture.Get(), RHIResourceState::RenderTarget);

    PostProcessStack stack;
    (void)stack.AddEffect<RecordingPostProcessPass>("ColorGrading", 100);

    stack.Execute(graph, input, output);

    const PostProcessStackExecuteStats& executeStats = stack.GetLastExecuteStats();
    EXPECT_FALSE(executeStats.toneMappingBoundaryValid);
    EXPECT_FALSE(executeStats.toneMappingBoundaryWarning.empty());
    EXPECT_EQ(executeStats.graphPassCount, 0u);
    EXPECT_EQ(executeStats.transientIntermediateCount, 0u);
    EXPECT_EQ(executeStats.finalOutputFormat, RHIFormat::RGBA8_UNORM);

    graph.Compile();
    const auto& graphStats = graph.GetCompileStats();
    EXPECT_TRUE(graphStats.compileValid);
    EXPECT_EQ(graphStats.totalPasses, 0u);
}

TEST_F(RenderPassValidationFixture, PostProcessStackReportsInvalidChromaticAberrationBeforeToneMapping)
{
    ASSERT_NO_FATAL_FAILURE(Initialize());

    RenderGraph graph;
    graph.SetDevice(&device);

    RHITextureRef inputTexture =
        device.CreateTexture(RHITextureDesc::RenderTarget(32, 32, RHIFormat::RGBA16_FLOAT));
    RHITextureRef outputTexture =
        device.CreateTexture(RHITextureDesc::RenderTarget(32, 32, RHIFormat::RGBA8_UNORM));
    ASSERT_TRUE(inputTexture);
    ASSERT_TRUE(outputTexture);

    RGTextureHandle input = graph.ImportTexture(inputTexture.Get(), RHIResourceState::ShaderResource);
    RGTextureHandle output = graph.ImportTexture(outputTexture.Get(), RHIResourceState::RenderTarget);

    PostProcessStack stack;
    (void)stack.AddEffect<RecordingPostProcessPass>("ChromaticAberration", 100);

    stack.Execute(graph, input, output);

    const PostProcessStackExecuteStats& executeStats = stack.GetLastExecuteStats();
    EXPECT_FALSE(executeStats.toneMappingBoundaryValid);
    EXPECT_FALSE(executeStats.toneMappingBoundaryWarning.empty());
    EXPECT_EQ(executeStats.graphPassCount, 0u);
    EXPECT_EQ(executeStats.transientIntermediateCount, 0u);
    EXPECT_EQ(executeStats.finalOutputFormat, RHIFormat::RGBA8_UNORM);

    graph.Compile();
    const auto& graphStats = graph.GetCompileStats();
    EXPECT_TRUE(graphStats.compileValid);
    EXPECT_EQ(graphStats.totalPasses, 0u);
}

TEST_F(RenderPassValidationFixture, PostProcessStackReportsInvalidVignetteBeforeToneMapping)
{
    ASSERT_NO_FATAL_FAILURE(Initialize());

    RenderGraph graph;
    graph.SetDevice(&device);

    RHITextureRef inputTexture =
        device.CreateTexture(RHITextureDesc::RenderTarget(32, 32, RHIFormat::RGBA16_FLOAT));
    RHITextureRef outputTexture =
        device.CreateTexture(RHITextureDesc::RenderTarget(32, 32, RHIFormat::RGBA8_UNORM));
    ASSERT_TRUE(inputTexture);
    ASSERT_TRUE(outputTexture);

    RGTextureHandle input = graph.ImportTexture(inputTexture.Get(), RHIResourceState::ShaderResource);
    RGTextureHandle output = graph.ImportTexture(outputTexture.Get(), RHIResourceState::RenderTarget);

    PostProcessStack stack;
    (void)stack.AddEffect<RecordingPostProcessPass>("Vignette", 100);

    stack.Execute(graph, input, output);

    const PostProcessStackExecuteStats& executeStats = stack.GetLastExecuteStats();
    EXPECT_FALSE(executeStats.toneMappingBoundaryValid);
    EXPECT_FALSE(executeStats.toneMappingBoundaryWarning.empty());
    EXPECT_EQ(executeStats.graphPassCount, 0u);
    EXPECT_EQ(executeStats.transientIntermediateCount, 0u);
    EXPECT_EQ(executeStats.finalOutputFormat, RHIFormat::RGBA8_UNORM);

    graph.Compile();
    const auto& graphStats = graph.GetCompileStats();
    EXPECT_TRUE(graphStats.compileValid);
    EXPECT_EQ(graphStats.totalPasses, 0u);
}

TEST_F(RenderPassValidationFixture, PostProcessStackEvaluateEffectsCountsRuntimeSupportedEffects)
{
    ASSERT_NO_FATAL_FAILURE(Initialize());

    PostProcessSettings settings;
    settings.enableBloom = true;
    settings.enableToneMapping = true;

    PostProcessStack stack;
    auto* bloom = stack.AddEffect<BloomPass>();
    auto* toneMapping = stack.AddEffect<ToneMappingPass>();
    bloom->Configure(settings);
    toneMapping->Configure(settings);
    bloom->SetResources(&pipelineCache, &viewCache);
    toneMapping->SetResources(&pipelineCache, &viewCache);

    const PostProcessStackExecuteStats stats = stack.EvaluateEffects();
    EXPECT_FALSE(stats.noEffectNoWork);
    EXPECT_EQ(stats.requestedEffectCount, 2u);
    EXPECT_EQ(stats.unsupportedSkippedCount, 0u);
    EXPECT_EQ(stats.enabledEffectCount, 2u);
    EXPECT_EQ(stats.graphPassCount, 0u);
    EXPECT_EQ(stats.transientIntermediateCount, 0u);
}

TEST(RenderPostProcessStackValidation, SceneRendererWiresLDREffectsWithoutFlippingRuntimeDefaults)
{
    const fs::path shaderDir = FindShaderDirectory();
    if (shaderDir.empty())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    const fs::path sceneRendererPath = shaderDir.parent_path() /
        "Private" / "Renderer" / "SceneRenderer.cpp";
    const std::string source = ReadTextFile(sceneRendererPath);

    EXPECT_NE(source.find("settings.enableColorGrading = false;"), std::string::npos);
    EXPECT_NE(source.find("settings.enableChromaticAberration = false;"), std::string::npos);
    EXPECT_NE(source.find("settings.enableFXAA = false;"), std::string::npos);
    EXPECT_NE(source.find("settings.enableVignette = false;"), std::string::npos);
    EXPECT_NE(source.find("m_colorGradingPostProcess = m_postProcessStack->AddEffect<ColorGradingPass>();"),
              std::string::npos);
    EXPECT_NE(source.find("m_colorGradingPostProcess->SetResources(m_pipelineCache.get(), m_resourceViewCache.get());"),
              std::string::npos);
    EXPECT_NE(source.find("m_chromaticAberrationPostProcess = m_postProcessStack->AddEffect<ChromaticAberrationPass>();"),
              std::string::npos);
    EXPECT_NE(source.find("m_chromaticAberrationPostProcess->SetResources(m_pipelineCache.get(), m_resourceViewCache.get());"),
              std::string::npos);
    EXPECT_NE(source.find("m_vignettePostProcess = m_postProcessStack->AddEffect<VignettePass>();"),
              std::string::npos);
    EXPECT_NE(source.find("m_vignettePostProcess->SetResources(m_pipelineCache.get(), m_resourceViewCache.get());"),
              std::string::npos);
    EXPECT_NE(source.find("m_fxaaPostProcess = m_postProcessStack->AddEffect<FXAAPass>();"),
              std::string::npos);
    EXPECT_NE(source.find("m_fxaaPostProcess->SetResources(m_pipelineCache.get(), m_resourceViewCache.get());"),
              std::string::npos);
}

TEST(RenderPostProcessStackValidation, EvaluateEffectsReportsRequestedButUnsupportedResources)
{
    PostProcessSettings settings;
    settings.enableBloom = true;
    settings.enableToneMapping = true;

    PostProcessStack stack;
    auto* bloom = stack.AddEffect<BloomPass>();
    auto* toneMapping = stack.AddEffect<ToneMappingPass>();
    bloom->Configure(settings);
    toneMapping->Configure(settings);

    const PostProcessStackExecuteStats stats = stack.EvaluateEffects();
    EXPECT_TRUE(stats.noEffectNoWork);
    EXPECT_EQ(stats.requestedEffectCount, 2u);
    EXPECT_EQ(stats.unsupportedSkippedCount, 2u);
    EXPECT_EQ(stats.enabledEffectCount, 0u);
    EXPECT_EQ(stats.graphPassCount, 0u);
}

TEST_F(RenderPassValidationFixture, NoSupportedEffectsReportsNoWork)
{
    RenderGraph graph;
    graph.SetDevice(&device);

    PostProcessStack stack;
    stack.Execute(graph, {}, {});

    const PostProcessStackExecuteStats& executeStats = stack.GetLastExecuteStats();
    EXPECT_TRUE(executeStats.noEffectNoWork);
    EXPECT_EQ(executeStats.enabledEffectCount, 0u);
    EXPECT_EQ(executeStats.graphPassCount, 0u);

    graph.Compile();
    const auto& graphStats = graph.GetCompileStats();
    EXPECT_TRUE(graphStats.compileValid);
    EXPECT_EQ(graphStats.totalPasses, 0u);
}

TEST(RenderPostProcessStackValidation, MultiPassChainUsesDistinctTransientIntermediate)
{
    FakeDevice device;
    RenderGraph graph;
    graph.SetDevice(&device);

    RHITextureRef inputTexture =
        device.CreateTexture(RHITextureDesc::RenderTarget(32, 32, RHIFormat::RGBA16_FLOAT));
    RHITextureRef outputTexture =
        device.CreateTexture(RHITextureDesc::RenderTarget(32, 32, RHIFormat::RGBA8_UNORM));
    ASSERT_TRUE(inputTexture);
    ASSERT_TRUE(outputTexture);

    RGTextureHandle input = graph.ImportTexture(inputTexture.Get(), RHIResourceState::ShaderResource);
    RGTextureHandle output = graph.ImportTexture(outputTexture.Get(), RHIResourceState::RenderTarget);
    graph.SetExportState(output, RHIResourceState::RenderTarget);

    PostProcessStack stack;
    auto* first = stack.AddEffect<RecordingPostProcessPass>("FirstPostProcess", 100);
    auto* second = stack.AddEffect<RecordingPostProcessPass>("SecondPostProcess", 200);

    stack.Execute(graph, input, output);

    const PostProcessStackExecuteStats& executeStats = stack.GetLastExecuteStats();
    EXPECT_FALSE(executeStats.noEffectNoWork);
    EXPECT_EQ(executeStats.enabledEffectCount, 2u);
    EXPECT_EQ(executeStats.graphPassCount, 2u);
    EXPECT_EQ(executeStats.transientIntermediateCount, 1u);

    EXPECT_EQ(first->addToGraphCount, 1u);
    EXPECT_EQ(second->addToGraphCount, 1u);
    EXPECT_EQ(first->lastInput.index, input.index);
    EXPECT_NE(first->lastOutput.index, input.index);
    EXPECT_NE(first->lastOutput.index, output.index);
    EXPECT_EQ(second->lastInput.index, first->lastOutput.index);
    EXPECT_EQ(second->lastOutput.index, output.index);

    const RHITextureDesc* intermediateDesc = graph.GetTextureDesc(first->lastOutput);
    ASSERT_NE(intermediateDesc, nullptr);
    EXPECT_EQ(intermediateDesc->width, 32u);
    EXPECT_EQ(intermediateDesc->height, 32u);
    EXPECT_EQ(intermediateDesc->format, RHIFormat::RGBA16_FLOAT);
    EXPECT_TRUE(HasFlag(intermediateDesc->usage, RHITextureUsage::RenderTarget));
    EXPECT_TRUE(HasFlag(intermediateDesc->usage, RHITextureUsage::ShaderResource));

    graph.Compile();
    const auto& graphStats = graph.GetCompileStats();
    EXPECT_TRUE(graphStats.compileValid);
    EXPECT_EQ(graphStats.totalPasses, 2u);
}

TEST_F(RenderPassValidationFixture, OpaquePassBindsOpaqueThenMaskedPipelinesAndDrawsBothGroups)
{
    ASSERT_NO_FATAL_FAILURE(Initialize());

    std::vector<RenderDrawItem> opaqueItems = {MakeDrawItem(MaterialRenderMode::Opaque)};
    std::vector<RenderDrawItem> maskedItems = {MakeDrawItem(MaterialRenderMode::Masked)};

    OpaquePass pass;
    pass.SetResources(&gpuResources, &pipelineCache, &materialSystem);
    pass.SetRenderScene(&scene, &opaqueItems, &maskedItems);
    pass.SetRenderTargets(colorView.Get(), nullptr);

    RecordingCommandContext ctx;
    pass.Execute(ctx, view);

    ASSERT_EQ(static_cast<size_t>(2), ctx.pipelineSequence.size());
    EXPECT_EQ(pipelineCache.GetOpaquePipeline(), ctx.pipelineSequence[0]);
    EXPECT_EQ(pipelineCache.GetMaskedPipeline(), ctx.pipelineSequence[1]);
    EXPECT_EQ(2u, ctx.drawIndexedCount);
}

TEST_F(RenderPassValidationFixture, OpaqueAndTransparentPassGateNormalMapsOnTangentBasis)
{
    const fs::path passesDir = FindShaderDirectory().parent_path() / "Private" / "Passes";
    ASSERT_FALSE(passesDir.empty());

    const std::string opaquePass = ReadTextFile(passesDir / "OpaquePass.cpp");
    const std::string transparentPass = ReadTextFile(passesDir / "TransparentPass.cpp");

    EXPECT_NE(opaquePass.find("MaterialBindingOptions materialOptions;"), std::string::npos);
    EXPECT_NE(opaquePass.find("materialOptions.allowNormalMap = buffers.HasNormalMapTangentBasis()"),
              std::string::npos);
    EXPECT_NE(opaquePass.find("PrepareMaterialBinding(materialResource, view.viewCache, materialOptions)"),
              std::string::npos);

    EXPECT_NE(transparentPass.find("MaterialBindingOptions materialOptions;"), std::string::npos);
    EXPECT_NE(transparentPass.find("materialOptions.allowNormalMap = buffers.HasNormalMapTangentBasis()"),
              std::string::npos);
    EXPECT_NE(transparentPass.find("PrepareMaterialBinding(materialResource, view.viewCache, materialOptions)"),
              std::string::npos);
}

TEST_F(RenderPassValidationFixture, OpaquePassResolvesRenderGraphColorTargetView)
{
    ASSERT_NO_FATAL_FAILURE(Initialize());

    RenderGraph graph;
    graph.SetDevice(&device);

    RHITextureDesc sceneColorDesc = RHITextureDesc::RenderTarget(64, 64, RHIFormat::RGBA8_UNORM);
    sceneColorDesc.debugName = "GraphSceneColorForOpaquePass";
    view.colorTarget = graph.CreateTexture(sceneColorDesc);
    graph.SetExportState(view.colorTarget, RHIResourceState::RenderTarget);
    view.renderGraph = &graph;
    view.viewCache = &viewCache;

    std::vector<RenderDrawItem> opaqueItems = {MakeDrawItem(MaterialRenderMode::Opaque)};
    std::vector<RenderDrawItem> maskedItems;

    OpaquePass pass;
    pass.SetResources(&gpuResources, &pipelineCache, &materialSystem);
    pass.SetRenderScene(&scene, &opaqueItems, &maskedItems);
    pass.SetRenderTargets(colorView.Get(), nullptr);

    pass.AddToGraph(graph, view);
    graph.Compile();

    RecordingCommandContext ctx;
    graph.Execute(ctx);

    ASSERT_FALSE(ctx.renderPasses.empty());
    ASSERT_GT(ctx.renderPasses[0].colorAttachmentCount, 0u);
    RHITextureView* resolvedView = ctx.renderPasses[0].colorAttachments[0].view;
    ASSERT_NE(resolvedView, nullptr);
    EXPECT_NE(resolvedView, colorView.Get());
    EXPECT_NE(resolvedView->GetTexture(), colorTexture.Get());
}

TEST_F(RenderPassValidationFixture, OpaquePassDeclaresDirectionalShadowReadDuringSetup)
{
    ASSERT_NO_FATAL_FAILURE(Initialize());

    RenderGraph graph;
    graph.SetDevice(&device);

    RHITextureDesc sceneColorDesc = RHITextureDesc::RenderTarget(64, 64, RHIFormat::RGBA8_UNORM);
    sceneColorDesc.debugName = "GraphSceneColorForOpaqueShadowPass";
    view.colorTarget = graph.CreateTexture(sceneColorDesc);
    graph.SetExportState(view.colorTarget, RHIResourceState::RenderTarget);
    view.renderGraph = &graph;
    view.viewCache = &viewCache;
    view.viewportWidth = 64;
    view.viewportHeight = 64;
    view.aspectRatio = 1.0f;
    view.fieldOfView = 1.0472f;
    view.nearPlane = 0.1f;
    view.farPlane = 100.0f;
    view.cameraPosition = Vec3(0.0f, 0.0f, 5.0f);
    view.cameraForward = Vec3(0.0f, 0.0f, -1.0f);
    view.inverseViewMatrix = Mat4Identity();

    ShadowPassConfig shadowConfig;
    shadowConfig.numCascades = 3;
    shadowConfig.shadowMapSize = 64;
    shadowConfig.filterRadiusTexels = 2.0f;
    shadowConfig.normalBias = 0.0375f;

    ShadowPass shadowPass;
    shadowPass.SetResources(&gpuResources, &pipelineCache);
    shadowPass.SetRenderScene(&scene);
    shadowPass.SetConfig(shadowConfig);
    shadowPass.SetDirectionalLight(Vec3{-0.3f, -1.0f, -0.2f}, Vec3{1.0f, 1.0f, 1.0f}, 1.0f);

    std::vector<RenderDrawItem> opaqueItems = {MakeDrawItem(MaterialRenderMode::Opaque)};
    std::vector<RenderDrawItem> maskedItems;

    OpaquePass opaquePass;
    opaquePass.SetResources(&gpuResources, &pipelineCache, &materialSystem);
    opaquePass.SetRenderScene(&scene, &opaqueItems, &maskedItems);
    opaquePass.SetRenderTargets(colorView.Get(), nullptr);
    opaquePass.SetDirectionalShadowSource(&shadowPass);

    shadowPass.AddToGraph(graph, view);
    opaquePass.AddToGraph(graph, view);

    EXPECT_TRUE(opaquePass.GetShadowStats().requested);
    EXPECT_TRUE(opaquePass.GetShadowStats().renderGraphReadDeclared);

    graph.Compile();
    const auto& stats = graph.GetCompileStats();
    EXPECT_TRUE(stats.compileValid);
    EXPECT_EQ(stats.totalPasses, 2u);
    EXPECT_EQ(stats.culledPasses, 0u);

    RecordingCommandContext ctx;
    graph.Execute(ctx);

    EXPECT_TRUE(opaquePass.GetShadowStats().frameShadowReady);
    const DirectionalShadowFrameBindingResult& binding =
        pipelineCache.GetLastDirectionalShadowFrameBindingResult();
    EXPECT_TRUE(binding.shadowSamplingEnabled);
    EXPECT_EQ(binding.fallbackReason, DirectionalShadowFallbackReason::None);

    const FakeBuffer* viewBuffer = FindCreatedBuffer(device, "ViewConstantBuffer");
    ASSERT_NE(viewBuffer, nullptr);
    ASSERT_GE(viewBuffer->GetStorage().size(), sizeof(ViewConstants));
    ViewConstants uploaded{};
    std::memcpy(&uploaded, viewBuffer->GetStorage().data(), sizeof(uploaded));
    EXPECT_FLOAT_EQ(uploaded.directionalShadowParams.x, 1.0f);
    EXPECT_FLOAT_EQ(uploaded.directionalShadowParams.y, shadowConfig.shadowBias);
    EXPECT_FLOAT_EQ(uploaded.directionalShadowParams.w,
                    shadowConfig.filterRadiusTexels / static_cast<float>(shadowConfig.shadowMapSize));
    EXPECT_FLOAT_EQ(uploaded.directionalShadowReceiverParams.x, shadowConfig.normalBias);
    EXPECT_FLOAT_EQ(uploaded.cameraForwardAndShadowCascadeCount.w, 3.0f);
    EXPECT_FALSE(IsIdentityMatrix(uploaded.directionalShadowViewProjections[0]));
    EXPECT_FALSE(IsIdentityMatrix(uploaded.directionalShadowViewProjections[1]));
    EXPECT_FALSE(IsIdentityMatrix(uploaded.directionalShadowViewProjections[2]));
    const auto& cascades = shadowPass.GetCascades();
    ASSERT_EQ(cascades.size(), static_cast<size_t>(3));
    const float splitRange = view.farPlane - view.nearPlane;
    EXPECT_FLOAT_EQ(uploaded.directionalShadowCascadeSplits.x,
                    view.nearPlane + cascades[0].splitDepth * splitRange);
    EXPECT_FLOAT_EQ(uploaded.directionalShadowCascadeSplits.y,
                    view.nearPlane + cascades[1].splitDepth * splitRange);
    EXPECT_FLOAT_EQ(uploaded.directionalShadowCascadeSplits.z,
                    view.nearPlane + cascades[2].splitDepth * splitRange);

    const auto shadowSrvIt = std::find_if(device.createdTextureViewDescs.begin(),
                                          device.createdTextureViewDescs.end(),
                                          [](const RHITextureViewDesc& desc)
                                          {
                                              return desc.debugName &&
                                                     std::string(desc.debugName) == "DirectionalShadowSRV";
                                          });
    ASSERT_NE(shadowSrvIt, device.createdTextureViewDescs.end());
    EXPECT_EQ(shadowSrvIt->type, RHITextureViewType::ShaderResource);
    EXPECT_EQ(shadowSrvIt->subresourceRange.baseArrayLayer, 0u);
    EXPECT_EQ(shadowSrvIt->subresourceRange.arrayLayerCount, RVX_ALL_LAYERS);
    EXPECT_EQ(shadowSrvIt->subresourceRange.aspect, RHITextureAspect::Depth);
}

TEST_F(RenderPassValidationFixture, OpaquePassReportsMissingShadowSRVWhenRequestedReadCannotResolveView)
{
    ASSERT_NO_FATAL_FAILURE(Initialize());

    RenderGraph graph;
    graph.SetDevice(&device);

    RHITextureDesc sceneColorDesc = RHITextureDesc::RenderTarget(64, 64, RHIFormat::RGBA8_UNORM);
    sceneColorDesc.debugName = "GraphSceneColorForOpaqueMissingShadowSRV";
    view.colorTarget = graph.CreateTexture(sceneColorDesc);
    graph.SetExportState(view.colorTarget, RHIResourceState::RenderTarget);
    view.renderGraph = &graph;
    view.viewCache = &viewCache;
    view.viewportWidth = 64;
    view.viewportHeight = 64;
    view.aspectRatio = 1.0f;
    view.fieldOfView = 1.0472f;
    view.nearPlane = 0.1f;
    view.farPlane = 100.0f;
    view.cameraPosition = Vec3(0.0f, 0.0f, 5.0f);
    view.cameraForward = Vec3(0.0f, 0.0f, -1.0f);
    view.inverseViewMatrix = Mat4Identity();

    ShadowPassConfig shadowConfig;
    shadowConfig.numCascades = 1;
    shadowConfig.shadowMapSize = 64;

    ShadowPass shadowPass;
    shadowPass.SetResources(&gpuResources, &pipelineCache);
    shadowPass.SetRenderScene(&scene);
    shadowPass.SetConfig(shadowConfig);
    shadowPass.SetDirectionalLight(Vec3{-0.3f, -1.0f, -0.2f}, Vec3{1.0f, 1.0f, 1.0f}, 1.0f);

    std::vector<RenderDrawItem> opaqueItems = {MakeDrawItem(MaterialRenderMode::Opaque)};
    std::vector<RenderDrawItem> maskedItems;

    OpaquePass opaquePass;
    opaquePass.SetResources(&gpuResources, &pipelineCache, &materialSystem);
    opaquePass.SetRenderScene(&scene, &opaqueItems, &maskedItems);
    opaquePass.SetRenderTargets(colorView.Get(), nullptr);
    opaquePass.SetDirectionalShadowSource(&shadowPass);

    shadowPass.AddToGraph(graph, view);
    opaquePass.AddToGraph(graph, view);
    EXPECT_TRUE(opaquePass.GetShadowStats().renderGraphReadDeclared);

    graph.Compile();

    device.failDirectionalShadowSRVCreation = true;
    RecordingCommandContext ctx;
    graph.Execute(ctx);

    EXPECT_FALSE(opaquePass.GetShadowStats().frameShadowReady);
    const DirectionalShadowFrameBindingResult& binding =
        pipelineCache.GetLastDirectionalShadowFrameBindingResult();
    EXPECT_FALSE(binding.shadowSamplingEnabled);
    EXPECT_EQ(binding.fallbackReason, DirectionalShadowFallbackReason::MissingShadowSRV);
}

TEST_F(RenderPassValidationFixture, OpaquePassSkipsMaskedItemsWhenMaskedPipelineIsMissing)
{
    ASSERT_NO_FATAL_FAILURE(Initialize());

    ASSERT_TRUE(pipelineCache.GetMaskedPipeline());
    pipelineCache.m_maskedPipeline.Reset();

    std::vector<RenderDrawItem> opaqueItems = {MakeDrawItem(MaterialRenderMode::Opaque)};
    std::vector<RenderDrawItem> maskedItems = {MakeDrawItem(MaterialRenderMode::Masked)};

    OpaquePass pass;
    pass.SetResources(&gpuResources, &pipelineCache, &materialSystem);
    pass.SetRenderScene(&scene, &opaqueItems, &maskedItems);
    pass.SetRenderTargets(colorView.Get(), nullptr);

    RecordingCommandContext ctx;
    pass.Execute(ctx, view);

    ASSERT_EQ(static_cast<size_t>(1), ctx.pipelineSequence.size());
    EXPECT_EQ(pipelineCache.GetOpaquePipeline(), ctx.pipelineSequence[0]);
    EXPECT_EQ(1u, ctx.drawIndexedCount);
}

TEST_F(RenderPassValidationFixture, OpaquePassSkipsOpaqueItemsWhenOpaquePipelineIsMissing)
{
    ASSERT_NO_FATAL_FAILURE(Initialize());

    ASSERT_TRUE(pipelineCache.GetOpaquePipeline());
    pipelineCache.m_opaquePipeline.Reset();

    std::vector<RenderDrawItem> opaqueItems = {MakeDrawItem(MaterialRenderMode::Opaque)};
    std::vector<RenderDrawItem> maskedItems = {MakeDrawItem(MaterialRenderMode::Masked)};

    OpaquePass pass;
    pass.SetResources(&gpuResources, &pipelineCache, &materialSystem);
    pass.SetRenderScene(&scene, &opaqueItems, &maskedItems);
    pass.SetRenderTargets(colorView.Get(), nullptr);

    RecordingCommandContext ctx;
    pass.Execute(ctx, view);

    ASSERT_EQ(static_cast<size_t>(1), ctx.pipelineSequence.size());
    EXPECT_EQ(pipelineCache.GetMaskedPipeline(), ctx.pipelineSequence[0]);
    EXPECT_EQ(1u, ctx.drawIndexedCount);
}

TEST_F(RenderPassValidationFixture, TransparentPassBindsTransparentPipeline)
{
    ASSERT_NO_FATAL_FAILURE(Initialize());

    std::vector<RenderDrawItem> transparentItems = {MakeDrawItem(MaterialRenderMode::Transparent)};

    TransparentPass pass;
    pass.SetResources(&gpuResources, &pipelineCache, &materialSystem);
    pass.SetRenderScene(&scene, &transparentItems);
    pass.SetRenderTargets(colorView.Get(), nullptr);

    RecordingCommandContext ctx;
    pass.Execute(ctx, view);

    ASSERT_EQ(static_cast<size_t>(1), ctx.pipelineSequence.size());
    EXPECT_EQ(pipelineCache.GetTransparentPipeline(), ctx.pipelineSequence[0]);
    EXPECT_EQ(1u, ctx.drawIndexedCount);
    EXPECT_FALSE(pipelineCache.GetLastDirectionalShadowFrameBindingResult().shadowSamplingEnabled);
    EXPECT_EQ(pipelineCache.GetLastDirectionalShadowFrameBindingResult().fallbackReason,
              DirectionalShadowFallbackReason::DisabledNoDirectionalLight);
}

TEST_F(RenderPassValidationFixture, OpaquePassSkipsDrawWhenMaterialBindingErrors)
{
    ASSERT_NO_FATAL_FAILURE(Initialize(false));

    std::vector<RenderDrawItem> opaqueItems = {MakeDrawItem(MaterialRenderMode::Opaque)};
    std::vector<RenderDrawItem> maskedItems;

    OpaquePass pass;
    pass.SetResources(&gpuResources, &pipelineCache, &materialSystem);
    pass.SetRenderScene(&scene, &opaqueItems, &maskedItems);
    pass.SetRenderTargets(colorView.Get(), nullptr);

    RecordingCommandContext ctx;
    pass.Execute(ctx, view);

    EXPECT_EQ(0u, ctx.drawIndexedCount);
    EXPECT_EQ(MaterialBindingStatus::Error, materialSystem.GetLastBindingResult().status);
    EXPECT_FALSE(materialSystem.GetLastBindingResult().IsDrawable());
    EXPECT_FALSE(std::any_of(ctx.descriptorSetSequence.begin(), ctx.descriptorSetSequence.end(),
                             [](uint32 set) { return set == 2; }));
}

TEST_F(RenderPassValidationFixture, OpaquePassDrawsWhenMaterialBindingUsesFallback)
{
    ASSERT_NO_FATAL_FAILURE(Initialize());

    Resource::TextureHandle missingTexture = CreateTextureResource(502);
    Resource::MaterialResource materialResource;
    ConfigureMaterialWithAlbedo(materialResource, missingTexture);

    RenderDrawItem item = MakeDrawItem(MaterialRenderMode::Opaque);
    item.materialResource = &materialResource;
    std::vector<RenderDrawItem> opaqueItems = {item};
    std::vector<RenderDrawItem> maskedItems;

    OpaquePass pass;
    pass.SetResources(&gpuResources, &pipelineCache, &materialSystem);
    pass.SetRenderScene(&scene, &opaqueItems, &maskedItems);
    pass.SetRenderTargets(colorView.Get(), nullptr);

    RecordingCommandContext ctx;
    pass.Execute(ctx, view);

    EXPECT_EQ(1u, ctx.drawIndexedCount);
    EXPECT_EQ(MaterialBindingStatus::Fallback, materialSystem.GetLastBindingResult().status);
    EXPECT_TRUE(materialSystem.GetLastBindingResult().IsDrawable());
    EXPECT_TRUE(std::any_of(ctx.descriptorSetSequence.begin(), ctx.descriptorSetSequence.end(),
                            [](uint32 set) { return set == 2; }));
}

TEST_F(RenderPassValidationFixture, TransparentPassSkipsDrawWhenMaterialBindingErrors)
{
    ASSERT_NO_FATAL_FAILURE(Initialize(false));

    std::vector<RenderDrawItem> transparentItems = {MakeDrawItem(MaterialRenderMode::Transparent)};

    TransparentPass pass;
    pass.SetResources(&gpuResources, &pipelineCache, &materialSystem);
    pass.SetRenderScene(&scene, &transparentItems);
    pass.SetRenderTargets(colorView.Get(), nullptr);

    RecordingCommandContext ctx;
    pass.Execute(ctx, view);

    EXPECT_EQ(0u, ctx.drawIndexedCount);
    EXPECT_EQ(MaterialBindingStatus::Error, materialSystem.GetLastBindingResult().status);
    EXPECT_FALSE(materialSystem.GetLastBindingResult().IsDrawable());
    EXPECT_FALSE(std::any_of(ctx.descriptorSetSequence.begin(), ctx.descriptorSetSequence.end(),
                             [](uint32 set) { return set == 2; }));
}

TEST_F(RenderPassValidationFixture, TransparentPassDrawsWhenMaterialBindingUsesFallback)
{
    ASSERT_NO_FATAL_FAILURE(Initialize());

    Resource::TextureHandle missingTexture = CreateTextureResource(503);
    Resource::MaterialResource materialResource;
    ConfigureMaterialWithAlbedo(materialResource, missingTexture);

    RenderDrawItem item = MakeDrawItem(MaterialRenderMode::Transparent);
    item.materialResource = &materialResource;
    std::vector<RenderDrawItem> transparentItems = {item};

    TransparentPass pass;
    pass.SetResources(&gpuResources, &pipelineCache, &materialSystem);
    pass.SetRenderScene(&scene, &transparentItems);
    pass.SetRenderTargets(colorView.Get(), nullptr);

    RecordingCommandContext ctx;
    pass.Execute(ctx, view);

    EXPECT_EQ(1u, ctx.drawIndexedCount);
    EXPECT_EQ(MaterialBindingStatus::Fallback, materialSystem.GetLastBindingResult().status);
    EXPECT_TRUE(materialSystem.GetLastBindingResult().IsDrawable());
    EXPECT_TRUE(std::any_of(ctx.descriptorSetSequence.begin(), ctx.descriptorSetSequence.end(),
                            [](uint32 set) { return set == 2; }));
}
