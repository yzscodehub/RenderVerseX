#include "Core/Log.h"
#include "Particle/GPU/CPUParticleSimulator.h"
#include "Particle/GPU/ParticleSorter.h"
#include "Particle/ParticleComponent.h"
#include "Particle/ParticleSystem.h"
#include "Particle/ParticleSystemInstance.h"
#include "Particle/ParticleSubsystem.h"
#include "Particle/ParticleSubsystemRenderAccess.h"
#include "Particle/Rendering/TrailRenderer.h"
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

    size_t FindCall(const std::vector<std::string>& calls, const char* name)
    {
        auto it = std::find(calls.begin(), calls.end(), name);
        return it == calls.end() ? calls.size() : static_cast<size_t>(std::distance(calls.begin(), it));
    }

    std::filesystem::path FindSourcePath(const std::filesystem::path& relativePath)
    {
        std::filesystem::path cursor = std::filesystem::current_path();
        for (uint32 i = 0; i < 8; ++i)
        {
            const std::filesystem::path candidate = cursor / relativePath;
            if (std::filesystem::exists(candidate))
            {
                return candidate;
            }

            if (!cursor.has_parent_path() || cursor == cursor.parent_path())
                break;

            cursor = cursor.parent_path();
        }

        return {};
    }

    std::string ReadSourceFile(const std::filesystem::path& relativePath)
    {
        const std::filesystem::path sourcePath = FindSourcePath(relativePath);
        if (!sourcePath.empty())
        {
            std::ifstream stream(sourcePath, std::ios::binary);
            return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
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

    std::string ReadParticleComponentSource()
    {
        return ReadSourceFile("Particle/Private/ParticleComponent.cpp");
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

TEST(ParticleValidation, ParticleSubsystemDefaultsToSnapshotPathWithoutLegacyPass)
{
    EnsureLogInitialized();
    FakeDevice device;
    ParticleSubsystem subsystem;
    ParticleSubsystemRenderAccess::SetDeviceForTesting(subsystem, &device);
    subsystem.GetConfig().enableGPUSimulation = false;
    subsystem.Initialize();

    EXPECT_FALSE(subsystem.IsRenderIntegrationReady());
    EXPECT_FALSE(subsystem.GetStatistics().renderPassRegistered);
    EXPECT_FALSE(subsystem.GetStatistics().preGraphCallbackRegistered);
    EXPECT_NE(subsystem.GetRenderIntegrationUnsupportedReason().find("Render-owned feature snapshots"),
              std::string::npos);

    auto system = ParticleSystem::CreateSimple("SnapshotProductionDefault");
    system->maxParticles = 16;
    ParticleSystemInstance* instance = subsystem.CreateInstance(system);
    ASSERT_NE(instance, nullptr);
    instance->Play();
    subsystem.Simulate(0.25f);
    ASSERT_GT(instance->GetAliveCount(), 0u);

    ParticleRenderSnapshot snapshot;
    EXPECT_TRUE(subsystem.BuildRenderSnapshot(snapshot));
    ASSERT_EQ(snapshot.items.size(), 1u);
    EXPECT_EQ(snapshot.items.front().payloadStatus, ParticleRenderSnapshotPayloadStatus::MetadataOnly);
    EXPECT_FALSE(snapshot.items.front().renderPayloadAvailable);
    EXPECT_NE(snapshot.items.front().renderPayloadReason.find("Render-owned particle draw data extraction"),
              std::string::npos);

    subsystem.Deinitialize();
}

TEST(ParticleValidation, ParticleSubsystemProductionDeviceAcquisitionSourceGuard)
{
    EnsureLogInitialized();
    const std::string subsystemSource = ReadParticleSubsystemSource();
    const std::string componentSource = ReadParticleComponentSource();
    ASSERT_FALSE(subsystemSource.empty());
    ASSERT_FALSE(componentSource.empty());

    EXPECT_EQ(subsystemSource.find("Engine/Engine.h"), std::string::npos);
    EXPECT_EQ(subsystemSource.find("Engine::Get"), std::string::npos);
    EXPECT_EQ(subsystemSource.find("#include \"Render/RenderSubsystem.h\""), std::string::npos);
    EXPECT_EQ(subsystemSource.find("#include \"Render/Renderer/SceneRenderer.h\""), std::string::npos);
    EXPECT_EQ(subsystemSource.find("RenderSubsystem*"), std::string::npos);
    EXPECT_EQ(subsystemSource.find("SceneRenderer*"), std::string::npos);
    EXPECT_EQ(subsystemSource.find("GetSubsystem<RenderSubsystem>"), std::string::npos);
    EXPECT_EQ(subsystemSource.find("SetRenderSubsystem"), std::string::npos);
    EXPECT_EQ(subsystemSource.find("->GetDevice()"), std::string::npos);
    EXPECT_EQ(subsystemSource.find("->GetSceneRenderer()"), std::string::npos);
    EXPECT_EQ(subsystemSource.find("ParticleSorter"), std::string::npos);
    EXPECT_EQ(subsystemSource.find("Particle/Rendering/ParticlePass.h"), std::string::npos);
    EXPECT_EQ(subsystemSource.find("Particle/Rendering/ParticleRenderer.h"), std::string::npos);
    EXPECT_EQ(subsystemSource.find("enableLegacyRenderPassRegistration"), std::string::npos);
    EXPECT_EQ(subsystemSource.find("AddParticlePass"), std::string::npos);
    EXPECT_EQ(subsystemSource.find("RemoveParticlePass"), std::string::npos);
    EXPECT_EQ(subsystemSource.find("AddPreGraphPrepareCallback"), std::string::npos);
    EXPECT_EQ(subsystemSource.find("RemovePreGraphPrepareCallback"), std::string::npos);
    EXPECT_NE(subsystemSource.find("Render-owned feature snapshots"), std::string::npos);

    EXPECT_EQ(componentSource.find("Engine/Engine.h"), std::string::npos);
    EXPECT_EQ(componentSource.find("Engine::Get"), std::string::npos);
    EXPECT_EQ(componentSource.find("GetSubsystem<ParticleSubsystem>"), std::string::npos);
    EXPECT_NE(componentSource.find("GetActiveSubsystem"), std::string::npos);
}

TEST(ParticleValidation, FeaturePublicHeadersDoNotIncludeRenderOrRHI)
{
    const std::vector<std::string> featureIncludeRoots = {
        "Particle/Include",
        "Terrain/Include",
        "Water/Include",
    };

    std::string violations;
    for (const std::string& featureIncludeRoot : featureIncludeRoots)
    {
        const std::filesystem::path includeRoot = FindSourcePath(featureIncludeRoot);
        ASSERT_FALSE(includeRoot.empty()) << featureIncludeRoot;

        for (const auto& entry : std::filesystem::recursive_directory_iterator(includeRoot))
        {
            if (!entry.is_regular_file() || entry.path().extension() != ".h")
                continue;

            std::ifstream stream(entry.path(), std::ios::binary);
            const std::string contents{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
            if (contents.find("#include \"Render/") != std::string::npos ||
                contents.find("#include <Render/") != std::string::npos ||
                contents.find("#include \"RHI/") != std::string::npos ||
                contents.find("#include <RHI/") != std::string::npos)
            {
                violations += featureIncludeRoot;
                violations += "/";
                violations += entry.path().lexically_relative(includeRoot).generic_string();
                violations += "\n";
            }
        }
    }

    EXPECT_TRUE(violations.empty()) << violations;
}

TEST(ParticleValidation, ParticleSubsystemPublicHeaderDoesNotExposeRenderOrRHITypes)
{
    const std::string subsystemHeader = ReadSourceFile("Particle/Include/Particle/ParticleSubsystem.h");
    ASSERT_FALSE(subsystemHeader.empty());

    EXPECT_EQ(subsystemHeader.find("IRHIDevice"), std::string::npos);
    EXPECT_EQ(subsystemHeader.find("RenderSubsystem"), std::string::npos);
    EXPECT_EQ(subsystemHeader.find("SceneRenderer"), std::string::npos);
    EXPECT_EQ(subsystemHeader.find("ViewData"), std::string::npos);
    EXPECT_EQ(subsystemHeader.find("class ParticleRenderer"), std::string::npos);
    EXPECT_EQ(subsystemHeader.find("ParticleRenderer*"), std::string::npos);
    EXPECT_EQ(subsystemHeader.find("ParticleRendererConfig"), std::string::npos);
    EXPECT_EQ(subsystemHeader.find("ParticleRendererDrawStats"), std::string::npos);
    EXPECT_EQ(subsystemHeader.find("ParticleSorter"), std::string::npos);
    EXPECT_EQ(subsystemHeader.find("ParticlePass"), std::string::npos);
    EXPECT_NE(subsystemHeader.find("ParticleRenderSnapshot"), std::string::npos);
}

TEST(ParticleValidation, TrailRendererBuildsCpuMeshWithoutRHI)
{
    EnsureLogInitialized();

    const std::string trailHeader =
        ReadSourceFile("Particle/Private/Particle/Rendering/TrailRenderer.h");
    const std::string trailSource =
        ReadSourceFile("Particle/Private/Rendering/TrailRenderer.cpp");
    ASSERT_FALSE(trailHeader.empty());
    ASSERT_FALSE(trailSource.empty());

    EXPECT_EQ(trailHeader.find("RHI/"), std::string::npos);
    EXPECT_EQ(trailHeader.find("IRHIDevice"), std::string::npos);
    EXPECT_EQ(trailHeader.find("RHICommandContext"), std::string::npos);
    EXPECT_EQ(trailHeader.find("RHIBuffer"), std::string::npos);
    EXPECT_EQ(trailSource.find("CreateBuffer"), std::string::npos);
    EXPECT_EQ(trailSource.find("SetVertexBuffer"), std::string::npos);
    EXPECT_EQ(trailSource.find("DrawIndexed"), std::string::npos);

    TrailRenderer trailRenderer;
    trailRenderer.Initialize(32);
    trailRenderer.BeginFrame();
    trailRenderer.AddTrailPoint(1, Vec3(0.0f, 0.0f, 0.0f), Vec3(1.0f, 0.0f, 0.0f), 1.0f, Vec4(1.0f));
    trailRenderer.EndFrame();
    EXPECT_TRUE(trailRenderer.GetVertices().empty());
    EXPECT_TRUE(trailRenderer.GetIndices().empty());
    EXPECT_EQ(trailRenderer.GetIndexCount(), 0u);

    trailRenderer.AddTrailPoint(1, Vec3(1.0f, 0.0f, 0.0f), Vec3(1.0f, 0.0f, 0.0f), 1.0f, Vec4(1.0f));
    trailRenderer.EndFrame();

    EXPECT_EQ(trailRenderer.GetVertices().size(), 4u);
    EXPECT_EQ(trailRenderer.GetIndices().size(), 6u);
    EXPECT_EQ(trailRenderer.GetIndexCount(), 6u);

    trailRenderer.Shutdown();
    EXPECT_TRUE(trailRenderer.GetVertices().empty());
    EXPECT_TRUE(trailRenderer.GetIndices().empty());
    EXPECT_EQ(trailRenderer.GetIndexCount(), 0u);
}

TEST(ParticleValidation, ParticleSorterReportsUnsupportedWithoutRHI)
{
    EnsureLogInitialized();

    const std::string sorterHeader =
        ReadSourceFile("Particle/Private/Particle/GPU/ParticleSorter.h");
    const std::string sorterSource =
        ReadSourceFile("Particle/Private/GPU/ParticleSorter.cpp");
    ASSERT_FALSE(sorterHeader.empty());
    ASSERT_FALSE(sorterSource.empty());

    EXPECT_EQ(sorterHeader.find("RHI/"), std::string::npos);
    EXPECT_EQ(sorterHeader.find("IRHIDevice"), std::string::npos);
    EXPECT_EQ(sorterHeader.find("RHICommandContext"), std::string::npos);
    EXPECT_EQ(sorterHeader.find("RHIBuffer"), std::string::npos);
    EXPECT_EQ(sorterSource.find("CreateBuffer"), std::string::npos);
    EXPECT_EQ(sorterSource.find("Dispatch("), std::string::npos);
    EXPECT_EQ(sorterSource.find("SetPipeline"), std::string::npos);

    ParticleSorter sorter;
    sorter.Initialize(128);
    EXPECT_TRUE(sorter.IsInitialized());
    EXPECT_FALSE(sorter.IsSupported());
    EXPECT_EQ(sorter.GetMaxParticles(), 128u);
    EXPECT_NE(sorter.GetUnsupportedReason().find("Render-owned"), std::string::npos);

    EXPECT_FALSE(sorter.Sort(12, Vec3(1.0f, 2.0f, 3.0f)));
    EXPECT_EQ(sorter.GetLastRequestedParticleCount(), 12u);
    EXPECT_NE(sorter.GetUnsupportedReason().find("unsupported in Particle"), std::string::npos);

    sorter.Shutdown();
    EXPECT_FALSE(sorter.IsInitialized());
    EXPECT_EQ(sorter.GetMaxParticles(), 0u);
    EXPECT_EQ(sorter.GetLastRequestedParticleCount(), 0u);
}

TEST(ParticleValidation, ParticleComponentUsesSubsystemOwnedInstanceWithSnapshotDefault)
{
    EnsureLogInitialized();
    FakeDevice device;
    ParticleSubsystem subsystem;
    ParticleSubsystemRenderAccess::SetDeviceForTesting(subsystem, &device);
    subsystem.GetConfig().enableGPUSimulation = false;
    subsystem.Initialize();
    ASSERT_FALSE(subsystem.IsRenderIntegrationReady());
    EXPECT_NE(subsystem.GetRenderIntegrationUnsupportedReason().find("Render-owned feature snapshots"),
              std::string::npos);

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
    EXPECT_EQ(subsystem.GetInstances().size(), 1u);

    component->SetParticleSystem(nullptr);
    EXPECT_EQ(component->GetInstance(), nullptr);
    EXPECT_EQ(component->GetInstanceOwnership(), ParticleInstanceOwnership::None);
    EXPECT_EQ(subsystem.GetInstances().size(), 0u);

    subsystem.Deinitialize();
}

TEST(ParticleValidation, ParticleComponentFallbackWithoutRenderReadySubsystemIsObservable)
{
    EnsureLogInitialized();
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

TEST(ParticleValidation, ParticleSubsystemBuildsRenderSnapshotWithoutRenderHandles)
{
    EnsureLogInitialized();
    FakeDevice device;
    ParticleSubsystem subsystem;
    ParticleSubsystemRenderAccess::SetDeviceForTesting(subsystem, &device);
    subsystem.GetConfig().enableGPUSimulation = false;
    subsystem.Initialize();

    auto liveSystem = ParticleSystem::CreateSimple("SnapshotParticles");
    liveSystem->id = 42;
    liveSystem->maxParticles = 32;
    liveSystem->renderMode = ParticleRenderMode::StretchedBillboard;
    liveSystem->blendMode = ParticleBlendMode::Additive;
    liveSystem->softParticleConfig.enabled = true;
    liveSystem->softParticleConfig.fadeDistance = 2.5f;

    ParticleSystemInstance* liveInstance = subsystem.CreateInstance(liveSystem);
    ASSERT_NE(liveInstance, nullptr);
    liveInstance->SetPosition(Vec3(1.0f, 2.0f, 3.0f));
    liveInstance->Play();
    subsystem.Simulate(0.25f);
    ASSERT_GT(liveInstance->GetAliveCount(), 0u);

    auto stoppedSystem = ParticleSystem::CreateSimple("StoppedSnapshotParticles");
    ASSERT_NE(subsystem.CreateInstance(stoppedSystem), nullptr);

    ParticleRenderSnapshot snapshot;
    EXPECT_TRUE(subsystem.BuildRenderSnapshot(snapshot));

    const ParticleRenderSnapshotMetadata metadata = snapshot.GetMetadata();
    EXPECT_EQ(metadata.schemaVersion, RVX_PARTICLE_RENDER_SNAPSHOT_SCHEMA_VERSION);
    EXPECT_EQ(metadata.status, ParticleRenderSnapshotStatus::Complete);
    EXPECT_TRUE(metadata.complete);
    EXPECT_EQ(metadata.itemCount, 1u);
    EXPECT_EQ(metadata.totalAliveParticles, liveInstance->GetAliveCount());
    EXPECT_EQ(metadata.skippedInstanceCount, 1u);

    ASSERT_EQ(snapshot.items.size(), 1u);
    const ParticleRenderSnapshotItem& item = snapshot.items.front();
    EXPECT_EQ(item.instanceId, liveInstance->GetInstanceId());
    EXPECT_EQ(item.systemId, 42u);
    EXPECT_EQ(item.systemName, "SnapshotParticles");
    EXPECT_EQ(item.renderMode, ParticleRenderSnapshotMode::StretchedBillboard);
    EXPECT_EQ(item.blendMode, ParticleRenderSnapshotBlendMode::Additive);
    EXPECT_EQ(item.simulationBackend, ParticleRenderSnapshotSimulationBackend::CPU);
    EXPECT_EQ(item.payloadStatus, ParticleRenderSnapshotPayloadStatus::MetadataOnly);
    EXPECT_EQ(item.aliveParticleCount, liveInstance->GetAliveCount());
    EXPECT_EQ(item.maxParticleCount, 32u);
    EXPECT_TRUE(item.visible);
    EXPECT_TRUE(item.simulationSupported);
    EXPECT_FALSE(item.renderPayloadAvailable);
    EXPECT_FALSE(item.sortingSupported);
    EXPECT_TRUE(item.softParticlesEnabled);
    EXPECT_FLOAT_EQ(item.softParticleFadeDistance, 2.5f);
    EXPECT_FLOAT_EQ(item.position.x, 1.0f);
    EXPECT_FLOAT_EQ(item.position.y, 2.0f);
    EXPECT_FLOAT_EQ(item.position.z, 3.0f);
    EXPECT_TRUE(item.unsupportedReason.empty());
    EXPECT_NE(item.renderPayloadReason.find("metadata only"), std::string::npos);
    EXPECT_NE(item.renderPayloadReason.find("Render-owned"), std::string::npos);
    EXPECT_NE(item.sortingReason.find("Render-owned"), std::string::npos);

    ASSERT_EQ(snapshot.skippedReasons.size(), 1u);
    EXPECT_NE(snapshot.skippedReasons.front().find("not playing"), std::string::npos);

    subsystem.Deinitialize();
}

TEST(ParticleValidation, ParticleSubsystemCreatesCpuSimulatorWhenDeviceIsInjected)
{
    EnsureLogInitialized();
    FakeDevice device;
    ParticleSubsystem subsystem;
    ParticleSubsystemRenderAccess::SetDeviceForTesting(subsystem, &device);
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
