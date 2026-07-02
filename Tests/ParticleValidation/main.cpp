#include "Core/Log.h"
#include "Engine/Engine.h"
#include "Particle/GPU/CPUParticleSimulator.h"
#include "Particle/ParticleComponent.h"
#include "Particle/ParticleSystem.h"
#include "Particle/ParticleSystemInstance.h"
#include "Particle/ParticleSubsystem.h"
#include "Particle/Rendering/ParticlePass.h"
#include "Particle/Rendering/ParticleRenderer.h"
#include "Render/Graph/RenderGraph.h"
#include "Render/Graph/ResourceViewCache.h"
#include "Render/PipelineCache.h"
#include "Render/Renderer/SceneRenderer.h"
#include "Render/Renderer/ViewData.h"
#include "RHI/RHI.h"
#include "RHI/RHICommandContext.h"
#include "RHI/RHIDevice.h"
#include "RHI/RHIRenderPass.h"
#include "Scene/SceneEntity.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <vector>

using namespace RVX;
using namespace RVX::Particle;

namespace
{
    class FakeBuffer final : public RHIBuffer
    {
    public:
        explicit FakeBuffer(const RHIBufferDesc& desc)
            : m_desc(desc)
            , m_storage(static_cast<size_t>(desc.size))
        {
        }

        uint64 GetSize() const override { return m_desc.size; }
        RHIBufferUsage GetUsage() const override { return m_desc.usage; }
        RHIMemoryType GetMemoryType() const override { return m_desc.memoryType; }
        uint32 GetStride() const override { return m_desc.stride; }
        void* Map() override { return m_storage.empty() ? nullptr : m_storage.data(); }
        void Unmap() override {}

    private:
        RHIBufferDesc m_desc;
        std::vector<uint8> m_storage;
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
        explicit FakePipeline(const RHIGraphicsPipelineDesc& desc)
            : debugName(desc.debugName ? desc.debugName : "")
        {
        }

        bool IsCompute() const override { return false; }

        std::string debugName;
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

    class RecordingCommandContext final : public RHICommandContext
    {
    public:
        void Begin() override { callSequence.push_back("Begin"); }
        void End() override { callSequence.push_back("End"); }
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
        void BeginRenderPass(const RHIRenderPassDesc& desc) override
        {
            renderPasses.push_back(desc);
            callSequence.push_back("BeginRenderPass");
        }
        void EndRenderPass() override { callSequence.push_back("EndRenderPass"); }
        void SetPipeline(RHIPipeline*) override { callSequence.push_back("SetPipeline"); }
        void SetVertexBuffer(uint32, RHIBuffer*, uint64 = 0) override { callSequence.push_back("SetVertexBuffer"); }
        void SetVertexBuffers(uint32, std::span<RHIBuffer* const>, std::span<const uint64> = {}) override {}
        void SetIndexBuffer(RHIBuffer*, RHIFormat format, uint64 = 0) override
        {
            indexBufferFormat = format;
            callSequence.push_back("SetIndexBuffer");
        }
        void SetDescriptorSet(uint32 set, RHIDescriptorSet*, std::span<const uint32> = {}) override
        {
            descriptorSetSlots.push_back(set);
            callSequence.push_back("SetDescriptorSet");
        }
        void SetPushConstants(const void*, uint32, uint32 = 0) override {}
        void SetViewport(const RHIViewport&) override { callSequence.push_back("SetViewport"); }
        void SetViewports(std::span<const RHIViewport>) override {}
        void SetScissor(const RHIRect&) override { callSequence.push_back("SetScissor"); }
        void SetScissors(std::span<const RHIRect>) override {}
        void Draw(uint32 vertexCount, uint32 instanceCount = 1, uint32 = 0, uint32 = 0) override
        {
            lastDrawVertexCount = vertexCount;
            lastDrawInstanceCount = instanceCount;
            callSequence.push_back("Draw");
        }
        void DrawIndexed(uint32 indexCount, uint32 instanceCount = 1, uint32 = 0, int32 = 0, uint32 = 0) override
        {
            lastDrawIndexCount = indexCount;
            lastDrawInstanceCount = instanceCount;
            callSequence.push_back("DrawIndexed");
        }
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

        std::vector<RHIRenderPassDesc> renderPasses;
        std::vector<uint32> descriptorSetSlots;
        std::vector<std::string> callSequence;
        RHIFormat indexBufferFormat = RHIFormat::Unknown;
        uint32 lastDrawVertexCount = 0;
        uint32 lastDrawIndexCount = 0;
        uint32 lastDrawInstanceCount = 0;
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
            return RHIBufferRef(new FakeBuffer(desc));
        }

        RHITextureRef CreateTexture(const RHITextureDesc& desc) override
        {
            createdTextureDescs.push_back(desc);
            return RHITextureRef(new FakeTexture(desc));
        }

        RHITextureViewRef CreateTextureView(RHITexture* texture, const RHITextureViewDesc& desc = {}) override
        {
            createdTextureViewDescs.push_back(desc);
            return RHITextureViewRef(new FakeTextureView(texture, desc));
        }

        RHISamplerRef CreateSampler(const RHISamplerDesc&) override { return RHISamplerRef(new FakeSampler()); }
        RHIShaderRef CreateShader(const RHIShaderDesc& desc) override { return RHIShaderRef(new FakeShader(desc)); }
        RHIHeapRef CreateHeap(const RHIHeapDesc&) override { return nullptr; }
        RHITextureRef CreatePlacedTexture(RHIHeap*, uint64, const RHITextureDesc&) override { return nullptr; }
        RHIBufferRef CreatePlacedBuffer(RHIHeap*, uint64, const RHIBufferDesc&) override { return nullptr; }
        MemoryRequirements GetTextureMemoryRequirements(const RHITextureDesc&) override { return {}; }
        MemoryRequirements GetBufferMemoryRequirements(const RHIBufferDesc&) override { return {}; }

        RHIDescriptorSetLayoutRef CreateDescriptorSetLayout(const RHIDescriptorSetLayoutDesc& desc) override
        {
            createdSetLayoutDescs.push_back(desc);
            return RHIDescriptorSetLayoutRef(new FakeDescriptorSetLayout(desc));
        }

        RHIPipelineLayoutRef CreatePipelineLayout(const RHIPipelineLayoutDesc& desc) override
        {
            createdPipelineLayoutDescs.push_back(desc);
            return RHIPipelineLayoutRef(new FakePipelineLayout());
        }

        RHIPipelineRef CreateGraphicsPipeline(const RHIGraphicsPipelineDesc& desc) override
        {
            createdGraphicsPipelineDescs.push_back(desc);
            if (failGraphicsPipelineCreation)
            {
                return nullptr;
            }
            return RHIPipelineRef(new FakePipeline(desc));
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
            if (signalFence)
            {
                signalFence->Signal(1);
                return 1;
            }
            return 0;
        }
        uint64 SubmitCommandContexts(std::span<RHICommandContext* const>, RHIFence* signalFence = nullptr) override
        {
            return SubmitCommandContext(nullptr, signalFence);
        }
        RHISwapChainRef CreateSwapChain(const RHISwapChainDesc&) override { return nullptr; }
        RHIFenceRef CreateFence(uint64 initialValue = 0) override { return RHIFenceRef(new FakeFence(initialValue)); }
        void WaitForFence(RHIFence*, uint64) override {}
        void WaitIdle() override {}
        void BeginFrame() override {}
        void EndFrame() override {}
        uint32 GetCurrentFrameIndex() const override { return 0; }
        RHIStagingBufferRef CreateStagingBuffer(const RHIStagingBufferDesc&) override { return nullptr; }
        RHIRingBufferRef CreateRingBuffer(const RHIRingBufferDesc&) override { return nullptr; }
        RHIMemoryStats GetMemoryStats() const override { return {}; }
        void BeginResourceGroup(const char*) override {}
        void EndResourceGroup() override {}
        const RHICapabilities& GetCapabilities() const override { return capabilities; }
        RHIBackendType GetBackendType() const override { return RHIBackendType::DX11; }

        RHICapabilities capabilities;
        bool failGraphicsPipelineCreation = false;
        std::vector<RHIBufferDesc> createdBufferDescs;
        std::vector<RHITextureDesc> createdTextureDescs;
        std::vector<RHITextureViewDesc> createdTextureViewDescs;
        std::vector<RHIDescriptorSetLayoutDesc> createdSetLayoutDescs;
        std::vector<RHIPipelineLayoutDesc> createdPipelineLayoutDescs;
        std::vector<RHIGraphicsPipelineDesc> createdGraphicsPipelineDescs;
        std::vector<RHIDescriptorSetDesc> createdDescriptorSetDescs;
    };

    ParticleRendererConfig MakeRendererConfig()
    {
        ParticleRendererConfig config;
        config.colorTargetFormat = RHIFormat::RGBA16_FLOAT;
        config.depthStencilFormat = PipelineCache::GetDefaultDepthStencilFormat();
        config.sampleCount = RHISampleCount::Count1;
        config.vertexShaderBytecode = {1, 2, 3, 4};
        config.pixelShaderBytecode = {5, 6, 7, 8};
        return config;
    }

    ViewData MakeView(RenderGraph* graph = nullptr, ResourceViewCache* cache = nullptr)
    {
        ViewData view;
        view.viewportWidth = 64;
        view.viewportHeight = 64;
        view.renderGraph = graph;
        view.viewCache = cache;
        return view;
    }

    ParticleSystemInstance MakeCpuInstance(FakeDevice& device, uint32 maxParticles = 32)
    {
        auto system = ParticleSystem::CreateSimple("ParticleValidation");
        system->maxParticles = maxParticles;
        system->renderMode = ParticleRenderMode::Billboard;
        system->blendMode = ParticleBlendMode::AlphaBlend;

        ParticleSystemInstance instance(system);
        auto simulator = std::make_unique<CPUParticleSimulator>();
        simulator->Initialize(&device, maxParticles);
        instance.SetSimulator(std::move(simulator), "CPU");
        instance.Play();
        instance.Simulate(0.25f);
        return instance;
    }

    void EnsureLogInitialized()
    {
        static bool initialized = false;
        if (!initialized)
        {
            Log::Initialize();
            initialized = true;
        }
    }

    const RHIBindingLayoutEntry* FindLayoutEntry(const RHIDescriptorSetLayoutDesc& desc, uint32 binding)
    {
        auto it = std::find_if(desc.entries.begin(), desc.entries.end(),
                               [binding](const RHIBindingLayoutEntry& entry)
                               {
                                   return entry.binding == binding;
                               });
        return it == desc.entries.end() ? nullptr : &*it;
    }

    const RHIDescriptorBinding* FindDescriptorBinding(const RHIDescriptorSetDesc& desc, uint32 binding)
    {
        auto it = std::find_if(desc.bindings.begin(), desc.bindings.end(),
                               [binding](const RHIDescriptorBinding& entry)
                               {
                                   return entry.binding == binding;
                               });
        return it == desc.bindings.end() ? nullptr : &*it;
    }

    size_t FindCall(const std::vector<std::string>& calls, const char* name)
    {
        auto it = std::find(calls.begin(), calls.end(), name);
        return it == calls.end() ? calls.size() : static_cast<size_t>(std::distance(calls.begin(), it));
    }

    std::string ReadSourceFile(const std::filesystem::path& relativePath)
    {
        std::filesystem::path cursor = std::filesystem::current_path();
        for (uint32 i = 0; i < 8; ++i)
        {
            const std::filesystem::path candidate = cursor / relativePath;
            if (std::filesystem::exists(candidate))
            {
                std::ifstream stream(candidate, std::ios::binary);
                return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
            }

            if (!cursor.has_parent_path() || cursor == cursor.parent_path())
                break;

            cursor = cursor.parent_path();
        }

        return {};
    }

    std::string ReadShaderSource()
    {
        return ReadSourceFile("Particle/Shaders/ParticleBillboard.hlsl");
    }

    std::string ReadSoftParticleSource()
    {
        return ReadSourceFile("Particle/Shaders/Include/SoftParticle.hlsli");
    }

    std::string ReadParticleCommonSource()
    {
        return ReadSourceFile("Particle/Shaders/Include/ParticleCommon.hlsli");
    }

    std::string ReadParticleTypesSource()
    {
        return ReadSourceFile("Particle/Include/Particle/ParticleTypes.h");
    }

    std::string ReadSceneRendererSource()
    {
        return ReadSourceFile("Render/Private/Renderer/SceneRenderer.cpp");
    }

    std::string ReadParticleSubsystemSource()
    {
        return ReadSourceFile("Particle/Private/ParticleSubsystem.cpp");
    }
} // namespace

TEST(ParticleValidation, SceneRendererPreGraphCallbacksUseOwnerTokens)
{
    EnsureLogInitialized();
    SceneRenderer renderer;
    int ownerA = 0;
    int ownerB = 0;
    uint32 callsA = 0;
    uint32 callsB = 0;

    EXPECT_FALSE(renderer.AddPreGraphPrepareCallback(nullptr, [](const ViewData&) {}));
    EXPECT_FALSE(renderer.AddPreGraphPrepareCallback(&ownerA, {}));
    EXPECT_TRUE(renderer.AddPreGraphPrepareCallback(
        &ownerA,
        [&callsA](const ViewData& view)
        {
            EXPECT_EQ(view.viewportWidth, 77u);
            ++callsA;
        }));
    EXPECT_FALSE(renderer.AddPreGraphPrepareCallback(&ownerA, [](const ViewData&) {}));
    EXPECT_TRUE(renderer.AddPreGraphPrepareCallback(
        &ownerB,
        [&callsB](const ViewData&)
        {
            ++callsB;
        }));
    EXPECT_EQ(renderer.GetPreGraphPrepareCallbackCount(), 2u);

    renderer.GetViewData().viewportWidth = 77;
    renderer.RunPreGraphPrepareCallbacksForTesting();
    EXPECT_EQ(callsA, 1u);
    EXPECT_EQ(callsB, 1u);

    EXPECT_TRUE(renderer.RemovePreGraphPrepareCallback(&ownerA));
    EXPECT_FALSE(renderer.RemovePreGraphPrepareCallback(&ownerA));
    EXPECT_EQ(renderer.GetPreGraphPrepareCallbackCount(), 1u);

    renderer.RunPreGraphPrepareCallbacksForTesting();
    EXPECT_EQ(callsA, 1u);
    EXPECT_EQ(callsB, 2u);

    EXPECT_TRUE(renderer.RemovePreGraphPrepareCallback(&ownerB));
    EXPECT_EQ(renderer.GetPreGraphPrepareCallbackCount(), 0u);
}

TEST(ParticleValidation, ParticleSubsystemRegistersPassAndCallbackIntoSceneRenderer)
{
    EnsureLogInitialized();
    FakeDevice device;
    SceneRenderer renderer;
    ParticleSubsystem subsystem;
    subsystem.SetDeviceForTesting(&device);
    subsystem.SetSceneRendererForTesting(&renderer);
    subsystem.SetRendererConfigForTesting(MakeRendererConfig());
    subsystem.GetConfig().enableGPUSimulation = false;
    subsystem.Initialize();

    EXPECT_TRUE(subsystem.IsRenderIntegrationReady()) << subsystem.GetRenderIntegrationUnsupportedReason();
    EXPECT_EQ(renderer.GetPassCount(), 1u);
    EXPECT_EQ(renderer.GetPreGraphPrepareCallbackCount(), 1u);
    EXPECT_NE(subsystem.GetRenderPass(), nullptr);
    EXPECT_TRUE(subsystem.GetStatistics().renderPassRegistered);
    EXPECT_TRUE(subsystem.GetStatistics().preGraphCallbackRegistered);

    auto system = ParticleSystem::CreateSimple("SubsystemRenderIntegration");
    system->maxParticles = 32;
    ParticleSystemInstance* instance = subsystem.CreateInstance(system);
    ASSERT_NE(instance, nullptr);
    instance->Play();
    subsystem.Simulate(0.25f);
    ASSERT_GT(instance->GetAliveCount(), 0u);

    renderer.GetViewData().cameraPosition = Vec3(0.0f, 0.0f, 0.0f);
    renderer.RunPreGraphPrepareCallbacksForTesting();
    EXPECT_EQ(subsystem.GetStatistics().prepareFrameCount, 1u);
    EXPECT_EQ(subsystem.GetStatistics().visibleInstances, 1u);
    EXPECT_EQ(subsystem.GetVisibleInstances().size(), 1u);

    subsystem.Deinitialize();
    EXPECT_FALSE(subsystem.IsRenderIntegrationReady());
    EXPECT_EQ(renderer.GetPreGraphPrepareCallbackCount(), 0u);
    EXPECT_EQ(renderer.GetPassCount(), 0u);
    const uint64 prepareFramesAfterDeinit = subsystem.GetStatistics().prepareFrameCount;
    renderer.RunPreGraphPrepareCallbacksForTesting();
    EXPECT_EQ(subsystem.GetStatistics().prepareFrameCount, prepareFramesAfterDeinit);
}

TEST(ParticleValidation, ParticleSubsystemDoesNotReportReadyWhenRendererUnsupported)
{
    EnsureLogInitialized();
    FakeDevice device;
    device.failGraphicsPipelineCreation = true;
    SceneRenderer renderer;
    ParticleSubsystem subsystem;
    subsystem.SetDeviceForTesting(&device);
    subsystem.SetSceneRendererForTesting(&renderer);
    subsystem.SetRendererConfigForTesting(MakeRendererConfig());
    subsystem.GetConfig().enableGPUSimulation = false;
    subsystem.Initialize();

    EXPECT_FALSE(subsystem.IsRenderIntegrationReady());
    EXPECT_EQ(subsystem.GetRenderPass(), nullptr);
    EXPECT_EQ(renderer.GetPassCount(), 0u);
    EXPECT_EQ(renderer.GetPreGraphPrepareCallbackCount(), 0u);
    EXPECT_FALSE(subsystem.GetStatistics().renderPassRegistered);
    EXPECT_FALSE(subsystem.GetStatistics().preGraphCallbackRegistered);
    EXPECT_NE(subsystem.GetRenderIntegrationUnsupportedReason().find("Particle renderer is unsupported"),
              std::string::npos);
    EXPECT_NE(subsystem.GetRenderIntegrationUnsupportedReason().find("pipeline"),
              std::string::npos);

    subsystem.Deinitialize();
}

TEST(ParticleValidation, ParticleSubsystemProductionDeviceAcquisitionSourceGuard)
{
    EnsureLogInitialized();
    const std::string source = ReadParticleSubsystemSource();
    ASSERT_FALSE(source.empty());

    EXPECT_NE(source.find("GetSubsystem<RenderSubsystem>"), std::string::npos);
    EXPECT_NE(source.find("GetDevice()"), std::string::npos);
    EXPECT_NE(source.find("GetSceneRenderer()"), std::string::npos);
    EXPECT_NE(source.find("AddPreGraphPrepareCallback"), std::string::npos);
    EXPECT_NE(source.find("RemovePreGraphPrepareCallback"), std::string::npos);
}

TEST(ParticleValidation, ParticleComponentUsesSubsystemOwnedInstanceWhenRenderReady)
{
    EnsureLogInitialized();
    Engine engine;
    FakeDevice device;
    SceneRenderer renderer;
    auto* subsystem = engine.AddSubsystem<ParticleSubsystem>();
    subsystem->SetDeviceForTesting(&device);
    subsystem->SetSceneRendererForTesting(&renderer);
    subsystem->SetRendererConfigForTesting(MakeRendererConfig());
    subsystem->GetConfig().enableGPUSimulation = false;
    subsystem->Initialize();
    ASSERT_TRUE(subsystem->IsRenderIntegrationReady()) << subsystem->GetRenderIntegrationUnsupportedReason();

    SceneEntity entity("ParticleComponentOwner");
    auto* component = entity.AddComponent<ParticleComponent>();
    ASSERT_NE(component, nullptr);

    auto system = ParticleSystem::CreateSimple("ComponentSubsystemPath");
    system->maxParticles = 32;
    component->SetParticleSystem(system);

    ASSERT_NE(component->GetInstance(), nullptr);
    EXPECT_EQ(component->GetInstanceOwnership(), ParticleInstanceOwnership::SubsystemOwned);
    EXPECT_FALSE(component->IsUsingLegacyFallback());
    EXPECT_TRUE(component->GetInstance()->IsSimulationSupported());
    EXPECT_EQ(subsystem->GetInstances().size(), 1u);

    component->SetParticleSystem(nullptr);
    EXPECT_EQ(component->GetInstance(), nullptr);
    EXPECT_EQ(component->GetInstanceOwnership(), ParticleInstanceOwnership::None);
    EXPECT_EQ(subsystem->GetInstances().size(), 0u);

    subsystem->Deinitialize();
}

TEST(ParticleValidation, ParticleComponentFallbackWithoutRenderReadySubsystemIsObservable)
{
    EnsureLogInitialized();
    Engine engine;
    SceneEntity entity("ParticleComponentFallbackOwner");
    auto* component = entity.AddComponent<ParticleComponent>();
    ASSERT_NE(component, nullptr);

    auto system = ParticleSystem::CreateSimple("ComponentFallbackPath");
    component->SetParticleSystem(system);

    ASSERT_NE(component->GetInstance(), nullptr);
    EXPECT_EQ(component->GetInstanceOwnership(), ParticleInstanceOwnership::LegacyFallback);
    EXPECT_TRUE(component->IsUsingLegacyFallback());
    EXPECT_FALSE(component->GetInstance()->IsSimulationSupported());

    component->SetParticleSystem(nullptr);
    EXPECT_EQ(component->GetInstance(), nullptr);
    EXPECT_EQ(component->GetInstanceOwnership(), ParticleInstanceOwnership::None);
}

TEST(ParticleValidation, ParticleSubsystemCreatesCpuSimulatorWhenDeviceIsInjected)
{
    EnsureLogInitialized();
    FakeDevice device;
    ParticleSubsystem subsystem;
    subsystem.SetDeviceForTesting(&device);
    subsystem.GetConfig().enableGPUSimulation = false;
    subsystem.Initialize();

    auto system = ParticleSystem::CreateSimple("SubsystemCpu");
    ParticleSystemInstance* instance = subsystem.CreateInstance(system);
    ASSERT_NE(instance, nullptr);
    EXPECT_TRUE(instance->IsSimulationSupported());
    EXPECT_EQ(instance->GetSimulationBackendName(), "CPU");
    ASSERT_NE(instance->GetSimulator(), nullptr);
    EXPECT_FALSE(instance->GetSimulator()->IsGPUBased());

    subsystem.Deinitialize();
}

TEST(ParticleValidation, CpuSimulationEmitsUploadsAndClears)
{
    EnsureLogInitialized();
    FakeDevice device;
    RecordingCommandContext ctx;
    ParticleSystemInstance instance = MakeCpuInstance(device);

    ASSERT_TRUE(instance.IsSimulationSupported());
    EXPECT_GT(instance.GetAliveCount(), 0u);
    ASSERT_NE(instance.GetSimulator(), nullptr);

    instance.GetSimulator()->PrepareRender(ctx);
    EXPECT_NE(instance.GetSimulator()->GetParticleBuffer(), nullptr);
    EXPECT_NE(instance.GetSimulator()->GetAliveIndexBuffer(), nullptr);

    instance.Clear();
    EXPECT_EQ(instance.GetAliveCount(), 0u);
    EXPECT_EQ(instance.GetSimulator()->GetAliveCount(), 0u);
}

TEST(ParticleValidation, ParticleBillboardShaderUsesUniqueRQ32Bindings)
{
    EnsureLogInitialized();
    const std::string source = ReadShaderSource();
    ASSERT_FALSE(source.empty());

    EXPECT_NE(source.find("register(b0)"), std::string::npos);
    EXPECT_NE(source.find("register(t1)"), std::string::npos);
    EXPECT_NE(source.find("register(t2)"), std::string::npos);
    EXPECT_NE(source.find("register(t3)"), std::string::npos);
    EXPECT_NE(source.find("register(s4)"), std::string::npos);
    EXPECT_EQ(source.find("register(t0)"), std::string::npos);
    EXPECT_EQ(source.find("RVX_PARTICLE_ENABLE_SOFT_PARTICLES"), std::string::npos);
    EXPECT_NE(source.find("g_Render.sceneDepthTestEnabled"), std::string::npos);
    EXPECT_NE(source.find("g_Render.softParticleEnabled"), std::string::npos);
    EXPECT_NE(source.find("g_Corners[6]"), std::string::npos);
    EXPECT_NE(source.find("float4 finalColor = input.color"), std::string::npos);
    EXPECT_EQ(source.find("g_ParticleTexture.Sample"), std::string::npos);
    EXPECT_NE(source.find("sceneViewDepth <= input.viewDepth"), std::string::npos);
    EXPECT_NE(source.find("g_Render.nearPlane"), std::string::npos);
    EXPECT_NE(source.find("g_Render.farPlane"), std::string::npos);
    EXPECT_EQ(source.find("1000.0"), std::string::npos);
}

TEST(ParticleValidation, ParticleSoftDepthSourceGuardrails)
{
    EnsureLogInitialized();

    const std::string softSource = ReadSoftParticleSource();
    ASSERT_FALSE(softSource.empty());
    EXPECT_NE(softSource.find("register(t5)"), std::string::npos);
    EXPECT_NE(softSource.find("register(s6)"), std::string::npos);
    EXPECT_EQ(softSource.find("register(t1)"), std::string::npos);
    EXPECT_EQ(softSource.find("register(s1)"), std::string::npos);
    EXPECT_EQ(softSource.find("register(b2)"), std::string::npos);
    EXPECT_NE(softSource.find("reverseZ"), std::string::npos);
    EXPECT_NE(softSource.find("fadeDistance"), std::string::npos);

    const std::string commonSource = ReadParticleCommonSource();
    const std::string typesSource = ReadParticleTypesSource();
    ASSERT_FALSE(commonSource.empty());
    ASSERT_FALSE(typesSource.empty());
    EXPECT_EQ(sizeof(RenderGPUData) % 16u, 0u);
    for (const char* field : {"sceneDepthTestEnabled", "nearPlane", "farPlane", "reverseZ"})
    {
        EXPECT_NE(commonSource.find(field), std::string::npos);
        EXPECT_NE(typesSource.find(field), std::string::npos);
    }

    const std::string sceneRendererSource = ReadSceneRendererSource();
    ASSERT_FALSE(sceneRendererSource.empty());
    EXPECT_NE(sceneRendererSource.find("RHITextureUsage::DepthStencil | RHITextureUsage::ShaderResource"),
              std::string::npos);
    EXPECT_NE(sceneRendererSource.find("SetExportState(m_viewData.depthTarget, RHIResourceState::DepthWrite)"),
              std::string::npos);
}

TEST(ParticleValidation, ParticleRendererCreatesDescriptorLayoutAndDrawsBillboards)
{
    EnsureLogInitialized();
    FakeDevice device;
    ParticleRenderer renderer;
    renderer.Initialize(&device, MakeRendererConfig());
    ASSERT_TRUE(renderer.IsRenderingSupported()) << renderer.GetUnsupportedReason();

    ASSERT_FALSE(device.createdSetLayoutDescs.empty());
    const RHIDescriptorSetLayoutDesc& layout = device.createdSetLayoutDescs.front();
    ASSERT_EQ(layout.entries.size(), 7u);
    ASSERT_NE(FindLayoutEntry(layout, 0), nullptr);
    EXPECT_EQ(FindLayoutEntry(layout, 0)->type, RHIBindingType::UniformBuffer);
    ASSERT_NE(FindLayoutEntry(layout, 1), nullptr);
    EXPECT_EQ(FindLayoutEntry(layout, 1)->type, RHIBindingType::ShaderResourceBuffer);
    ASSERT_NE(FindLayoutEntry(layout, 2), nullptr);
    EXPECT_EQ(FindLayoutEntry(layout, 2)->type, RHIBindingType::ShaderResourceBuffer);
    ASSERT_NE(FindLayoutEntry(layout, 3), nullptr);
    EXPECT_EQ(FindLayoutEntry(layout, 3)->type, RHIBindingType::SampledTexture);
    ASSERT_NE(FindLayoutEntry(layout, 4), nullptr);
    EXPECT_EQ(FindLayoutEntry(layout, 4)->type, RHIBindingType::Sampler);
    ASSERT_NE(FindLayoutEntry(layout, 5), nullptr);
    EXPECT_EQ(FindLayoutEntry(layout, 5)->type, RHIBindingType::SampledTexture);
    ASSERT_NE(FindLayoutEntry(layout, 6), nullptr);
    EXPECT_EQ(FindLayoutEntry(layout, 6)->type, RHIBindingType::Sampler);

    ParticleSystemInstance instance = MakeCpuInstance(device);
    RecordingCommandContext ctx;
    instance.GetSimulator()->PrepareRender(ctx);
    const uint32 aliveCount = instance.GetAliveCount();

    EXPECT_TRUE(renderer.DrawParticles(ctx, &instance, MakeView(), nullptr));
    ASSERT_FALSE(device.createdDescriptorSetDescs.empty());
    const RHIDescriptorSetDesc& descriptorSet = device.createdDescriptorSetDescs.back();
    const RHIDescriptorBinding* renderConstants = FindDescriptorBinding(descriptorSet, 0);
    const RHIDescriptorBinding* particleBuffer = FindDescriptorBinding(descriptorSet, 1);
    const RHIDescriptorBinding* aliveBuffer = FindDescriptorBinding(descriptorSet, 2);
    const RHIDescriptorBinding* fallbackTexture = FindDescriptorBinding(descriptorSet, 3);
    const RHIDescriptorBinding* sampler = FindDescriptorBinding(descriptorSet, 4);
    const RHIDescriptorBinding* depthTexture = FindDescriptorBinding(descriptorSet, 5);
    const RHIDescriptorBinding* depthSampler = FindDescriptorBinding(descriptorSet, 6);

    ASSERT_NE(renderConstants, nullptr);
    ASSERT_NE(particleBuffer, nullptr);
    ASSERT_NE(aliveBuffer, nullptr);
    ASSERT_NE(fallbackTexture, nullptr);
    ASSERT_NE(sampler, nullptr);
    ASSERT_NE(depthTexture, nullptr);
    ASSERT_NE(depthSampler, nullptr);
    EXPECT_NE(renderConstants->buffer, nullptr);
    EXPECT_EQ(particleBuffer->buffer, instance.GetSimulator()->GetParticleBuffer());
    EXPECT_EQ(aliveBuffer->buffer, instance.GetSimulator()->GetAliveIndexBuffer());
    EXPECT_NE(fallbackTexture->textureView, nullptr);
    EXPECT_NE(sampler->sampler, nullptr);
    EXPECT_NE(depthTexture->textureView, nullptr);
    EXPECT_NE(depthSampler->sampler, nullptr);
    EXPECT_FALSE(renderer.GetLastDrawStats().usedRealSceneDepth);
    EXPECT_FALSE(renderer.GetLastDrawStats().sceneDepthTestEnabled);
    EXPECT_FALSE(renderer.GetLastDrawStats().softParticlesEnabled);
    EXPECT_TRUE(renderer.GetLastDrawStats().drawSubmitted);
    EXPECT_FALSE(renderer.GetLastDrawStats().indexedDraw);
    EXPECT_FALSE(renderer.GetLastDrawStats().indirectDraw);
    EXPECT_EQ(renderer.GetLastDrawStats().submittedVertexCount, 6u);
    EXPECT_EQ(renderer.GetLastDrawStats().submittedIndexCount, 0u);
    EXPECT_EQ(renderer.GetLastDrawStats().submittedInstanceCount, aliveCount);
    EXPECT_NE(renderer.GetLastDrawStats().softParticleFallbackReason.find("Scene depth SRV unavailable"),
              std::string::npos);

    EXPECT_EQ(ctx.lastDrawVertexCount, 6u);
    EXPECT_EQ(ctx.lastDrawIndexCount, 0u);
    EXPECT_EQ(ctx.lastDrawInstanceCount, aliveCount);
    EXPECT_LT(FindCall(ctx.callSequence, "SetDescriptorSet"), FindCall(ctx.callSequence, "SetPipeline"));
}

TEST(ParticleValidation, ParticleRendererCreatesFixedAndShaderDepthPipelines)
{
    EnsureLogInitialized();
    FakeDevice device;
    ParticleRenderer renderer;
    renderer.Initialize(&device, MakeRendererConfig());
    ASSERT_TRUE(renderer.IsRenderingSupported()) << renderer.GetUnsupportedReason();

    uint32 fixedDepthPipelines = 0;
    uint32 shaderDepthPipelines = 0;
    for (const RHIGraphicsPipelineDesc& desc : device.createdGraphicsPipelineDescs)
    {
        if (desc.depthStencilFormat == PipelineCache::GetDefaultDepthStencilFormat())
        {
            ++fixedDepthPipelines;
            EXPECT_TRUE(desc.depthStencilState.depthTestEnable);
            EXPECT_FALSE(desc.depthStencilState.depthWriteEnable);
        }
        else if (desc.depthStencilFormat == RHIFormat::Unknown)
        {
            ++shaderDepthPipelines;
            EXPECT_FALSE(desc.depthStencilState.depthTestEnable);
            EXPECT_FALSE(desc.depthStencilState.depthWriteEnable);
        }
    }

    EXPECT_EQ(fixedDepthPipelines, 2u);
    EXPECT_EQ(shaderDepthPipelines, 2u);
}

TEST(ParticleValidation, ParticleRendererBindsRealDepthSrvForShaderDepthPath)
{
    EnsureLogInitialized();
    FakeDevice device;
    ParticleRenderer renderer;
    renderer.Initialize(&device, MakeRendererConfig());
    ASSERT_TRUE(renderer.IsRenderingSupported()) << renderer.GetUnsupportedReason();

    RHITextureRef sceneDepth = device.CreateTexture(
        RHITextureDesc::DepthStencil(64, 64, PipelineCache::GetDefaultDepthStencilFormat()));
    RHITextureViewDesc sceneDepthSrvDesc;
    sceneDepthSrvDesc.format = sceneDepth->GetFormat();
    sceneDepthSrvDesc.dimension = sceneDepth->GetDimension();
    sceneDepthSrvDesc.type = RHITextureViewType::ShaderResource;
    sceneDepthSrvDesc.subresourceRange.aspect = RHITextureAspect::Depth;
    RHITextureViewRef sceneDepthSrv = device.CreateTextureView(sceneDepth.Get(), sceneDepthSrvDesc);
    ASSERT_NE(sceneDepthSrv, nullptr);

    ParticleSystemInstance softInstance = MakeCpuInstance(device);
    RecordingCommandContext softCtx;
    softInstance.GetSimulator()->PrepareRender(softCtx);
    EXPECT_TRUE(renderer.DrawParticles(softCtx,
                                       &softInstance,
                                       MakeView(),
                                       sceneDepthSrv.Get(),
                                       ParticleDepthMode::ShaderDepth,
                                       true));

    ASSERT_FALSE(device.createdDescriptorSetDescs.empty());
    const RHIDescriptorSetDesc& softDescriptorSet = device.createdDescriptorSetDescs.back();
    const RHIDescriptorBinding* softDepth = FindDescriptorBinding(softDescriptorSet, 5);
    ASSERT_NE(softDepth, nullptr);
    EXPECT_EQ(softDepth->textureView, sceneDepthSrv.Get());
    EXPECT_TRUE(renderer.GetLastDrawStats().usedRealSceneDepth);
    EXPECT_TRUE(renderer.GetLastDrawStats().sceneDepthTestEnabled);
    EXPECT_TRUE(renderer.GetLastDrawStats().softParticlesEnabled);
    EXPECT_TRUE(renderer.GetLastDrawStats().softParticleFallbackReason.empty());
    EXPECT_EQ(renderer.GetLastDrawStats().depthMode, ParticleDepthMode::ShaderDepth);

    ParticleSystemInstance hardInstance = MakeCpuInstance(device);
    hardInstance.GetSystem()->softParticleConfig.enabled = false;
    RecordingCommandContext hardCtx;
    hardInstance.GetSimulator()->PrepareRender(hardCtx);
    EXPECT_TRUE(renderer.DrawParticles(hardCtx,
                                       &hardInstance,
                                       MakeView(),
                                       sceneDepthSrv.Get(),
                                       ParticleDepthMode::ShaderDepth,
                                       true));

    const RHIDescriptorSetDesc& hardDescriptorSet = device.createdDescriptorSetDescs.back();
    const RHIDescriptorBinding* hardDepth = FindDescriptorBinding(hardDescriptorSet, 5);
    ASSERT_NE(hardDepth, nullptr);
    EXPECT_EQ(hardDepth->textureView, sceneDepthSrv.Get());
    EXPECT_TRUE(renderer.GetLastDrawStats().usedRealSceneDepth);
    EXPECT_TRUE(renderer.GetLastDrawStats().sceneDepthTestEnabled);
    EXPECT_FALSE(renderer.GetLastDrawStats().softParticlesEnabled);
    EXPECT_NE(renderer.GetLastDrawStats().softParticleFallbackReason.find("ParticleSystem"),
              std::string::npos);
}

TEST(ParticleValidation, ParticleRendererRejectsInvalidPipelineConfig)
{
    EnsureLogInitialized();

    FakeDevice unknownColorDevice;
    ParticleRenderer unknownColorRenderer;
    ParticleRendererConfig unknownColorConfig = MakeRendererConfig();
    unknownColorConfig.colorTargetFormat = RHIFormat::Unknown;
    unknownColorRenderer.Initialize(&unknownColorDevice, unknownColorConfig);
    EXPECT_FALSE(unknownColorRenderer.IsRenderingSupported());
    EXPECT_NE(unknownColorRenderer.GetUnsupportedReason().find("color render target format"), std::string::npos);
    EXPECT_TRUE(unknownColorDevice.createdGraphicsPipelineDescs.empty());

    FakeDevice invalidDepthDevice;
    ParticleRenderer invalidDepthRenderer;
    ParticleRendererConfig invalidDepthConfig = MakeRendererConfig();
    invalidDepthConfig.depthStencilFormat = RHIFormat::RGBA8_UNORM;
    invalidDepthRenderer.Initialize(&invalidDepthDevice, invalidDepthConfig);
    EXPECT_FALSE(invalidDepthRenderer.IsRenderingSupported());
    EXPECT_NE(invalidDepthRenderer.GetUnsupportedReason().find("depthStencilFormat"), std::string::npos);
    EXPECT_TRUE(invalidDepthDevice.createdGraphicsPipelineDescs.empty());
}

TEST(ParticleValidation, SupportedParticleRendererRejectsOutOfScopeModesWithoutDrawing)
{
    EnsureLogInitialized();
    FakeDevice device;
    ParticleRenderer renderer;
    renderer.Initialize(&device, MakeRendererConfig());
    ASSERT_TRUE(renderer.IsRenderingSupported()) << renderer.GetUnsupportedReason();

    ParticleSystemInstance meshInstance = MakeCpuInstance(device);
    meshInstance.GetSystem()->renderMode = ParticleRenderMode::Mesh;
    RecordingCommandContext meshCtx;
    meshInstance.GetSimulator()->PrepareRender(meshCtx);
    EXPECT_FALSE(renderer.DrawParticles(meshCtx, &meshInstance, MakeView(), nullptr));
    EXPECT_EQ(meshCtx.lastDrawVertexCount, 0u);
    EXPECT_EQ(meshCtx.lastDrawIndexCount, 0u);

    ParticleSystemInstance multiplyInstance = MakeCpuInstance(device);
    multiplyInstance.GetSystem()->blendMode = ParticleBlendMode::Multiply;
    RecordingCommandContext blendCtx;
    multiplyInstance.GetSimulator()->PrepareRender(blendCtx);
    EXPECT_FALSE(renderer.DrawParticles(blendCtx, &multiplyInstance, MakeView(), nullptr));
    EXPECT_EQ(blendCtx.lastDrawVertexCount, 0u);
    EXPECT_EQ(blendCtx.lastDrawIndexCount, 0u);
}

TEST(ParticleValidation, ParticlePassUsesDepthSrvColorOnlyPathWhenAvailable)
{
    EnsureLogInitialized();
    FakeDevice device;
    ParticleRenderer renderer;
    renderer.Initialize(&device, MakeRendererConfig());
    ASSERT_TRUE(renderer.IsRenderingSupported()) << renderer.GetUnsupportedReason();

    ParticleSystemInstance instance = MakeCpuInstance(device);
    ParticlePass pass;
    pass.SetRenderer(&renderer);
    pass.SetParticleSystems(std::vector<ParticleSystemInstance*>{&instance});

    RenderGraph graph;
    graph.SetDevice(&device);
    ResourceViewCache viewCache;
    viewCache.Initialize(&device);

    RHITextureDesc colorDesc = RHITextureDesc::RenderTarget(64, 64, RHIFormat::RGBA16_FLOAT);
    RHITextureDesc depthDesc = RHITextureDesc::DepthStencil(64, 64, PipelineCache::GetDefaultDepthStencilFormat());
    RHITextureRef colorTexture = device.CreateTexture(colorDesc);
    RHITextureRef depthTexture = device.CreateTexture(depthDesc);
    ViewData view = MakeView(&graph, &viewCache);
    view.colorTarget = graph.ImportTexture(colorTexture.Get(), RHIResourceState::RenderTarget);
    view.depthTarget = graph.ImportTexture(depthTexture.Get(), RHIResourceState::DepthRead);
    graph.SetExportState(view.colorTarget, RHIResourceState::RenderTarget);

    graph.AddPass<int>(
        "ParticlePassValidation",
        RenderGraphPassType::Graphics,
        [&](RenderGraphBuilder& builder, int&)
        {
            pass.Setup(builder, view);
        },
        [&](const int&, RHICommandContext& ctx)
        {
            pass.Execute(ctx, view);
        });

    graph.Compile();
    RecordingCommandContext ctx;
    graph.Execute(ctx);

    ASSERT_EQ(ctx.renderPasses.size(), 1u);
    EXPECT_EQ(ctx.renderPasses.front().colorAttachmentCount, 1u);
    EXPECT_FALSE(ctx.renderPasses.front().hasDepthStencil);
    EXPECT_EQ(ctx.lastDrawVertexCount, 6u);
    EXPECT_EQ(ctx.lastDrawIndexCount, 0u);
    EXPECT_EQ(ctx.lastDrawInstanceCount, instance.GetAliveCount());
    EXPECT_TRUE(renderer.GetLastDrawStats().usedRealSceneDepth);
    EXPECT_TRUE(renderer.GetLastDrawStats().sceneDepthTestEnabled);
    EXPECT_TRUE(renderer.GetLastDrawStats().softParticlesEnabled);
    EXPECT_EQ(renderer.GetLastDrawStats().depthMode, ParticleDepthMode::ShaderDepth);

    ASSERT_FALSE(device.createdDescriptorSetDescs.empty());
    const RHIDescriptorBinding* depthBinding =
        FindDescriptorBinding(device.createdDescriptorSetDescs.back(), 5);
    ASSERT_NE(depthBinding, nullptr);
    ASSERT_NE(depthBinding->textureView, nullptr);
    EXPECT_EQ(depthBinding->textureView->GetTexture(), depthTexture.Get());

    EXPECT_LT(FindCall(ctx.callSequence, "BeginRenderPass"), FindCall(ctx.callSequence, "SetViewport"));
    EXPECT_LT(FindCall(ctx.callSequence, "SetViewport"), FindCall(ctx.callSequence, "SetScissor"));
    EXPECT_LT(FindCall(ctx.callSequence, "SetScissor"), FindCall(ctx.callSequence, "SetDescriptorSet"));
    EXPECT_LT(FindCall(ctx.callSequence, "SetDescriptorSet"), FindCall(ctx.callSequence, "SetPipeline"));
    EXPECT_LT(FindCall(ctx.callSequence, "SetPipeline"), FindCall(ctx.callSequence, "Draw"));
    EXPECT_LT(FindCall(ctx.callSequence, "Draw"), FindCall(ctx.callSequence, "EndRenderPass"));
}

TEST(ParticleValidation, ParticlePassUsesReadOnlyDsvWhenSoftParticlesAreDisabled)
{
    EnsureLogInitialized();
    FakeDevice device;
    ParticleRenderer renderer;
    renderer.Initialize(&device, MakeRendererConfig());
    ASSERT_TRUE(renderer.IsRenderingSupported()) << renderer.GetUnsupportedReason();

    ParticleSystemInstance instance = MakeCpuInstance(device);
    ParticlePass pass;
    pass.SetRenderer(&renderer);
    pass.SetSoftParticlesEnabled(false);
    pass.SetParticleSystems(std::vector<ParticleSystemInstance*>{&instance});

    RenderGraph graph;
    graph.SetDevice(&device);
    ResourceViewCache viewCache;
    viewCache.Initialize(&device);

    RHITextureDesc colorDesc = RHITextureDesc::RenderTarget(64, 64, RHIFormat::RGBA16_FLOAT);
    RHITextureDesc depthDesc = RHITextureDesc::DepthStencil(64, 64, PipelineCache::GetDefaultDepthStencilFormat());
    RHITextureRef colorTexture = device.CreateTexture(colorDesc);
    RHITextureRef depthTexture = device.CreateTexture(depthDesc);
    ViewData view = MakeView(&graph, &viewCache);
    view.colorTarget = graph.ImportTexture(colorTexture.Get(), RHIResourceState::RenderTarget);
    view.depthTarget = graph.ImportTexture(depthTexture.Get(), RHIResourceState::DepthRead);
    graph.SetExportState(view.colorTarget, RHIResourceState::RenderTarget);

    graph.AddPass<int>(
        "ParticlePassDsvWhenSoftDisabledValidation",
        RenderGraphPassType::Graphics,
        [&](RenderGraphBuilder& builder, int&)
        {
            pass.Setup(builder, view);
        },
        [&](const int&, RHICommandContext& ctx)
        {
            pass.Execute(ctx, view);
        });

    graph.Compile();
    RecordingCommandContext ctx;
    graph.Execute(ctx);

    ASSERT_EQ(ctx.renderPasses.size(), 1u);
    EXPECT_EQ(ctx.renderPasses.front().colorAttachmentCount, 1u);
    EXPECT_TRUE(ctx.renderPasses.front().hasDepthStencil);
    EXPECT_TRUE(ctx.renderPasses.front().depthStencilAttachment.readOnly);
    EXPECT_EQ(ctx.lastDrawVertexCount, 6u);
    EXPECT_EQ(ctx.lastDrawIndexCount, 0u);
    EXPECT_EQ(ctx.lastDrawInstanceCount, instance.GetAliveCount());
    EXPECT_EQ(renderer.GetLastDrawStats().depthMode, ParticleDepthMode::FixedFunction);
    EXPECT_FALSE(renderer.GetLastDrawStats().usedRealSceneDepth);
    EXPECT_FALSE(renderer.GetLastDrawStats().sceneDepthTestEnabled);
    EXPECT_FALSE(renderer.GetLastDrawStats().softParticlesEnabled);
}

TEST(ParticleValidation, ParticlePassFallsBackToReadOnlyDsvWhenDepthSrvUnavailable)
{
    EnsureLogInitialized();
    FakeDevice device;
    ParticleRenderer renderer;
    renderer.Initialize(&device, MakeRendererConfig());
    ASSERT_TRUE(renderer.IsRenderingSupported()) << renderer.GetUnsupportedReason();

    ParticleSystemInstance instance = MakeCpuInstance(device);
    ParticlePass pass;
    pass.SetRenderer(&renderer);
    pass.SetParticleSystems(std::vector<ParticleSystemInstance*>{&instance});

    RenderGraph graph;
    graph.SetDevice(&device);
    ResourceViewCache viewCache;
    viewCache.Initialize(&device);

    RHITextureDesc colorDesc = RHITextureDesc::RenderTarget(64, 64, RHIFormat::RGBA16_FLOAT);
    RHITextureDesc depthDesc = RHITextureDesc::Texture2D(64,
                                                        64,
                                                        PipelineCache::GetDefaultDepthStencilFormat(),
                                                        RHITextureUsage::DepthStencil);
    RHITextureRef colorTexture = device.CreateTexture(colorDesc);
    RHITextureRef depthTexture = device.CreateTexture(depthDesc);
    ViewData view = MakeView(&graph, &viewCache);
    view.colorTarget = graph.ImportTexture(colorTexture.Get(), RHIResourceState::RenderTarget);
    view.depthTarget = graph.ImportTexture(depthTexture.Get(), RHIResourceState::DepthRead);
    graph.SetExportState(view.colorTarget, RHIResourceState::RenderTarget);

    graph.AddPass<int>(
        "ParticlePassDsvFallbackValidation",
        RenderGraphPassType::Graphics,
        [&](RenderGraphBuilder& builder, int&)
        {
            pass.Setup(builder, view);
        },
        [&](const int&, RHICommandContext& ctx)
        {
            pass.Execute(ctx, view);
        });

    graph.Compile();
    RecordingCommandContext ctx;
    graph.Execute(ctx);

    ASSERT_EQ(ctx.renderPasses.size(), 1u);
    EXPECT_EQ(ctx.renderPasses.front().colorAttachmentCount, 1u);
    EXPECT_TRUE(ctx.renderPasses.front().hasDepthStencil);
    EXPECT_TRUE(ctx.renderPasses.front().depthStencilAttachment.readOnly);
    EXPECT_EQ(ctx.lastDrawVertexCount, 6u);
    EXPECT_EQ(ctx.lastDrawIndexCount, 0u);
    EXPECT_EQ(ctx.lastDrawInstanceCount, instance.GetAliveCount());
    EXPECT_EQ(renderer.GetLastDrawStats().depthMode, ParticleDepthMode::FixedFunction);
    EXPECT_FALSE(renderer.GetLastDrawStats().usedRealSceneDepth);
    EXPECT_FALSE(renderer.GetLastDrawStats().sceneDepthTestEnabled);
    EXPECT_FALSE(renderer.GetLastDrawStats().softParticlesEnabled);
    EXPECT_NE(renderer.GetLastDrawStats().softParticleFallbackReason.find("Scene depth SRV unavailable"),
              std::string::npos);

    ASSERT_FALSE(device.createdDescriptorSetDescs.empty());
    const RHIDescriptorBinding* depthBinding =
        FindDescriptorBinding(device.createdDescriptorSetDescs.back(), 5);
    ASSERT_NE(depthBinding, nullptr);
    ASSERT_NE(depthBinding->textureView, nullptr);
    EXPECT_NE(depthBinding->textureView->GetTexture(), depthTexture.Get());
}
