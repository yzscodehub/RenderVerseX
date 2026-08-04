#include "Core/Log.h"
#include "Core/MathTypes.h"
#include "Render/Material/MaterialClassification.h"
#include "RHI/RHI.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <iterator>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#define private public
#include "Render/PipelineCache.h"
#include "Render/GPUDriven/GPUCulling.h"
#undef private

#include "Common/DeterministicShaderCompiler.h"
#include "Common/RenderRuntimeTestHarness.h"
#include "Render/Debug/DebugRenderer.h"
#include "Render/Decal/DecalRenderer.h"
#include "Render/GPUScene/GPUSceneSchema.h"
#include "Render/Graph/ResourceViewCache.h"
#include "Render/Lighting/ClusteredLighting.h"
#include "Render/Lighting/LightManager.h"
#include "Render/Material/MaterialSystem.h"
#include "Render/Passes/DepthPrepass.h"
#include "Render/Passes/IRenderPass.h"
#include "Render/Passes/MeshPassProcessor.h"
#include "Render/Passes/ObjectVelocityPass.h"
#include "Render/Passes/OpaquePass.h"
#include "Render/Passes/ParticleFeaturePass.h"
#include "Render/Passes/RayTracedReflectionCompositePass.h"
#include "Render/Passes/RayTracedReflectionDenoisePass.h"
#include "Render/Passes/RayTracedReflectionPass.h"
#include "Render/Passes/RayTracedShadowPass.h"
#include "Render/Passes/ShadowPass.h"
#include "Render/Passes/SkyboxPass.h"
#include "Render/Passes/TransparentPass.h"
#include "Render/PostProcess/Bloom.h"
#include "Render/PostProcess/ChromaticAberration.h"
#include "Render/PostProcess/ColorGrading.h"
#include "Render/PostProcess/DOF.h"
#include "Render/PostProcess/FilmGrain.h"
#include "Render/PostProcess/FXAA.h"
#include "Render/PostProcess/MotionBlur.h"
#include "Render/PostProcess/PostProcessStack.h"
#include "Render/PostProcess/SSAO.h"
#include "Render/PostProcess/SSR.h"
#include "Render/PostProcess/TAA.h"
#include "Render/PostProcess/ToneMapping.h"
#include "Render/PostProcess/Vignette.h"
#include "Render/PostProcess/VolumetricLighting.h"
#include "Render/RayTracing/RayTracingResourceBindings.h"
#include "Render/RayTracing/RayTracingScene.h"
#include "Render/RayTracing/RayTracingSceneManager.h"
#include "Render/Renderer/RenderScene.h"
#include "Render/Renderer/SceneRenderer.h"
#include "Render/Renderer/ViewData.h"
#include "Render/Visibility/RenderVisibility.h"
#include "Render/Policy/RenderFramePlanCompiler.h"
#include "Render/Policy/RenderPolicyResolver.h"
#include "Render/SwapChainManager.h"
#include "Renderer/RenderPassRegistry.h"
#include "Resources/RenderResourceRegistry.h"
#include "Resources/RenderRetirementQueue.h"
#include "Resources/RenderSubmissionResourceBatch.h"
#include "Resources/RenderSubmissionTracker.h"
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
    namespace RTShadowBindings = RayTracingResourceBindings::Shadow;
    namespace RTReflectionBindings = RayTracingResourceBindings::Reflection;

#define RVX_REQUIRE_RENDER_RUNTIME_PIPELINE(...) \
    ASSERT_NO_FATAL_FAILURE(Initialize(__VA_ARGS__))

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

    std::string ToTestContentHashString(uint64 hash)
    {
        constexpr char digits[] = "0123456789abcdef";
        std::string text(16, '0');
        for (int32 i = 15; i >= 0; --i)
        {
            text[static_cast<size_t>(i)] = digits[hash & 0x0F];
            hash >>= 4;
        }
        return text;
    }

    std::string ComputeTestContentHash(const std::string& text)
    {
        uint64 hash = 14695981039346656037ull;
        for (char ch : text)
        {
            hash ^= static_cast<uint8>(ch);
            hash *= 1099511628211ull;
        }
        return ToTestContentHashString(hash);
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
        void SetMapSucceeds(bool succeeds) { m_mapSucceeds = succeeds; }
        void SetReportedSize(uint64 size) { m_desc.size = size; }
        void SetReportedStride(uint32 stride) { m_desc.stride = stride; }

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

    class FakeAccelerationStructure final : public RHIAccelerationStructure
    {
    public:
        explicit FakeAccelerationStructure(const RHIAccelerationStructureDesc& desc)
            : m_desc(desc)
            , m_address(s_nextAddress)
        {
            s_nextAddress += 0x1000;
        }

        RHIAccelerationStructureType GetType() const override { return m_desc.type; }
        uint64 GetSize() const override { return m_desc.size; }
        uint64 GetGPUVirtualAddress() const override { return m_address; }

    private:
        RHIAccelerationStructureDesc m_desc;
        uint64 m_address = 0;
        inline static uint64 s_nextAddress = 0x1000;
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

        explicit FakePipeline(const RHIComputePipelineDesc& desc)
            : debugName(desc.debugName ? desc.debugName : "")
            , m_compute(true)
        {
        }

        explicit FakePipeline(const RHIRayTracingPipelineDesc& desc)
            : debugName(desc.debugName ? desc.debugName : "")
            , m_rayTracing(true)
        {
            m_shaderGroupStages.reserve(desc.shaderGroups.size());
            m_shaderGroupIsHitGroup.reserve(desc.shaderGroups.size());
            for (const RHIRayTracingShaderGroupDesc& group : desc.shaderGroups)
            {
                if (group.type == RHIRayTracingShaderGroupType::General)
                {
                    m_shaderGroupStages.push_back(group.generalShader ? group.generalShader->GetStage() : RHIShaderStage::None);
                    m_shaderGroupIsHitGroup.push_back(0);
                }
                else
                {
                    m_shaderGroupStages.push_back(RHIShaderStage::None);
                    m_shaderGroupIsHitGroup.push_back(1);
                }
            }
        }

        bool IsCompute() const override { return m_compute; }
        bool IsRayTracing() const override { return m_rayTracing; }
        uint32 GetRayTracingShaderGroupCount() const override
        {
            return static_cast<uint32>(m_shaderGroupStages.size());
        }
        RHIShaderStage GetRayTracingShaderGroupStage(uint32 shaderGroupIndex) const override
        {
            return shaderGroupIndex < m_shaderGroupStages.size()
                ? m_shaderGroupStages[shaderGroupIndex]
                : RHIShaderStage::None;
        }
        bool IsRayTracingHitGroup(uint32 shaderGroupIndex) const override
        {
            return shaderGroupIndex < m_shaderGroupIsHitGroup.size() && m_shaderGroupIsHitGroup[shaderGroupIndex] != 0;
        }

        std::string debugName;

    private:
        bool m_compute = false;
        bool m_rayTracing = false;
        std::vector<RHIShaderStage> m_shaderGroupStages;
        std::vector<uint8> m_shaderGroupIsHitGroup;
    };


    class FakeShaderTable final : public RHIShaderTable
    {
    public:
        explicit FakeShaderTable(const RHIShaderTableDesc& desc)
            : m_pipelineOwner(desc.rayTracingPipelineOwner)
            , m_pipeline(desc.GetRayTracingPipeline())
            , m_rayGenerationRecordCount(static_cast<uint32>(desc.rayGenerationRecords.size()))
            , m_missRecordCount(static_cast<uint32>(desc.missRecords.size()))
            , m_hitGroupRecordCount(static_cast<uint32>(desc.hitGroupRecords.size()))
            , m_callableRecordCount(static_cast<uint32>(desc.callableRecords.size()))
        {
        }

        uint32 GetRayGenerationRecordCount() const override { return m_rayGenerationRecordCount; }
        uint32 GetMissRecordCount() const override { return m_missRecordCount; }
        uint32 GetHitGroupRecordCount() const override { return m_hitGroupRecordCount; }
        uint32 GetCallableRecordCount() const override { return m_callableRecordCount; }
        RHIPipeline* GetRayTracingPipeline() const override { return m_pipeline; }

    private:
        RHIPipelineRef m_pipelineOwner;
        RHIPipeline* m_pipeline = nullptr;
        uint32 m_rayGenerationRecordCount = 0;
        uint32 m_missRecordCount = 0;
        uint32 m_hitGroupRecordCount = 0;
        uint32 m_callableRecordCount = 0;
    };

    class FakeQueryPool final : public RHIQueryPool
    {
    public:
        explicit FakeQueryPool(const RHIQueryPoolDesc& desc)
            : m_type(desc.type)
            , m_count(desc.count)
        {
        }

        RHIQueryType GetType() const override { return m_type; }
        uint32 GetCount() const override { return m_count; }
        uint64 GetTimestampFrequency() const override
        {
            return m_type == RHIQueryType::Timestamp ? 1000000ull : 0ull;
        }

    private:
        RHIQueryType m_type = RHIQueryType::Timestamp;
        uint32 m_count = 0;
    };

    struct FakeDescriptorSetLifetimeState
    {
        bool alive = true;
    };

    class FakeDescriptorSet final : public RHIDescriptorSet
    {
    public:
        explicit FakeDescriptorSet(const RHIDescriptorSetDesc& desc)
            : RHIDescriptorSet(desc)
            , bindings(desc.bindings)
        {
        }

        ~FakeDescriptorSet() override
        {
            lifetime->alive = false;
        }

        bool Update(const std::vector<RHIDescriptorBinding>& newBindings) override
        {
            bindings = newBindings;
            return true;
        }

        std::vector<RHIDescriptorBinding> bindings;
        std::shared_ptr<FakeDescriptorSetLifetimeState> lifetime =
            std::make_shared<FakeDescriptorSetLifetimeState>();
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
        explicit RecordingCommandContext(
            RHICommandQueueType queueType = RHICommandQueueType::Graphics)
            : m_queueType(queueType)
        {
        }

        RHICommandQueueType GetQueueType() const override { return m_queueType; }
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
            currentPipeline = pipeline;
            pipelineSequence.push_back(pipeline);
            callSequence.push_back("SetPipeline");
        }
        void SetVertexBuffer(uint32 slot,
                             RHIBuffer* buffer,
                             uint64 = 0) override
        {
            if (slot < currentVertexBuffers.size())
            {
                currentVertexBuffers[slot] = buffer;
            }
        }
        void SetVertexBuffers(uint32, std::span<RHIBuffer* const>, std::span<const uint64> = {}) override {}
        void SetIndexBuffer(RHIBuffer*, RHIFormat, uint64 = 0) override {}
        void SetDescriptorSet(uint32 set,
                              RHIDescriptorSet* descriptorSet,
                              std::span<const uint32> dynamicOffsets = {}) override
        {
            descriptorSetSequence.push_back(set);
            descriptorSetPointers.push_back(descriptorSet);
            descriptorSetDynamicOffsets.emplace_back(dynamicOffsets.begin(),
                                                     dynamicOffsets.end());
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
        void DrawIndexed(uint32 indexCount,
                         uint32 instanceCount = 1,
                         uint32 firstIndex = 0,
                         int32 vertexOffset = 0,
                         uint32 firstInstance = 0) override
        {
            (void)indexCount;
            (void)instanceCount;
            (void)vertexOffset;
            (void)firstInstance;
            ++drawIndexedCount;
            drawIndexedFirstIndices.push_back(firstIndex);
            drawIndexedVertexBuffers.push_back(currentVertexBuffers);
        }
        void DrawIndirect(RHIBuffer*, uint64, uint32, uint32) override {}
        void DrawIndexedIndirect(RHIBuffer* buffer, uint64 offset, uint32 indirectDrawCount, uint32 stride) override
        {
            ++drawIndexedIndirectCount;
            lastIndirectBuffer = buffer;
            lastIndirectOffset = offset;
            lastIndirectDrawCount = indirectDrawCount;
            lastIndirectStride = stride;
            callSequence.push_back("DrawIndexedIndirect");
        }
        void Dispatch(uint32, uint32, uint32) override {}
        void DispatchIndirect(RHIBuffer*, uint64) override {}
        void BuildBottomLevelAccelerationStructure(
            RHIAccelerationStructure* dst,
            const RHIBottomLevelASDesc&,
            RHIBuffer* scratchBuffer,
            uint64 = 0,
            RHIAccelerationStructure* = nullptr) override
        {
            ++buildBottomLevelASCount;
            lastBottomLevelAS = dst;
            lastBottomLevelScratch = scratchBuffer;
            callSequence.push_back("BuildBottomLevelAS");
        }
        void BuildTopLevelAccelerationStructure(
            RHIAccelerationStructure* dst,
            const RHITopLevelASDesc& desc,
            RHIBuffer* scratchBuffer,
            uint64 = 0,
            RHIAccelerationStructure* = nullptr) override
        {
            ++buildTopLevelASCount;
            lastTopLevelAS = dst;
            lastTopLevelDesc = desc;
            lastTopLevelScratch = scratchBuffer;
            callSequence.push_back("BuildTopLevelAS");
        }
        void DispatchRays(const RHIDispatchRaysDesc& desc) override
        {
            ++dispatchRaysCount;
            lastDispatchRaysDesc = desc;
            lastDispatchRaysValidation = ValidateRHIDispatchRaysDesc(desc, currentPipeline);
            callSequence.push_back("DispatchRays");
        }
        void CopyBuffer(RHIBuffer*, RHIBuffer*, uint64, uint64, uint64) override { ++copyBufferCount; }
        void CopyTexture(RHITexture*, RHITexture*, const RHITextureCopyDesc& = {}) override { ++copyTextureCount; }
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
        void SetDepthBias(float, float, float = 0.0f) override { ++depthBiasSetCount; }
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
        uint32 copyTextureCount = 0;
        uint32 copyBufferToTextureCount = 0;
        uint32 depthBiasSetCount = 0;
        uint32 drawCount = 0;
        uint32 drawIndexedCount = 0;
        std::vector<uint32> drawIndexedFirstIndices;
        std::array<RHIBuffer*, 7> currentVertexBuffers{};
        std::vector<std::array<RHIBuffer*, 7>> drawIndexedVertexBuffers;
        uint32 drawIndexedIndirectCount = 0;
        uint32 buildBottomLevelASCount = 0;
        uint32 buildTopLevelASCount = 0;
        uint32 dispatchRaysCount = 0;
        uint32 lastDrawVertexCount = 0;
        RHIBuffer* lastIndirectBuffer = nullptr;
        uint64 lastIndirectOffset = 0;
        uint32 lastIndirectDrawCount = 0;
        uint32 lastIndirectStride = 0;
        RHIAccelerationStructure* lastBottomLevelAS = nullptr;
        RHIAccelerationStructure* lastTopLevelAS = nullptr;
        RHIBuffer* lastBottomLevelScratch = nullptr;
        RHIBuffer* lastTopLevelScratch = nullptr;
        RHITopLevelASDesc lastTopLevelDesc;
        RHIDispatchRaysDesc lastDispatchRaysDesc;
        RHIRayTracingValidationResult lastDispatchRaysValidation;
        RHIPipeline* currentPipeline = nullptr;
        std::vector<RHIRenderPassDesc> renderPasses;
        std::vector<RHIPipeline*> pipelineSequence;
        std::vector<uint32> descriptorSetSequence;
        std::vector<RHIDescriptorSet*> descriptorSetPointers;
        std::vector<std::vector<uint32>> descriptorSetDynamicOffsets;
        std::vector<RHIViewport> viewports;
        std::vector<RHIRect> scissors;
        std::vector<std::string> callSequence;

    private:
        RHICommandQueueType m_queueType = RHICommandQueueType::Graphics;
    };

    class FakeFence final : public RHIFence
    {
    public:
        explicit FakeFence(uint64 initialValue, bool autoComplete)
            : m_completedValue(initialValue)
            , m_autoComplete(autoComplete)
        {
        }

        uint64 GetCompletedValue() const override { return m_completedValue; }
        void Signal(uint64 value) override
        {
            m_signaledValue = value;
            if (m_autoComplete)
            {
                m_completedValue = value;
            }
        }
        void SignalOnQueue(uint64 value, RHICommandQueueType) override { Signal(value); }
        void Wait(uint64 value, uint64 = UINT64_MAX) override { m_completedValue = value; }
        void Complete(uint64 value) { m_completedValue = value; }
        uint64 GetLastSignaledValue() const { return m_signaledValue; }

    private:
        uint64 m_completedValue = 0;
        uint64 m_signaledValue = 0;
        bool m_autoComplete = true;
    };

    class FakeDevice final : public IRHIDevice
    {
    public:
        FakeDevice()
        {
            m_capabilities.backendType = RHIBackendType::DX12;
            m_capabilities.adapterName = "RenderPassValidation";
            m_capabilities.driverVersion = "1";
            m_capabilities.supportsComputePipeline = true;
            m_capabilities.supportsDescriptorSets = true;
            m_capabilities.supportsDynamicDescriptorOffsets = true;
            m_capabilities.maxDescriptorSets = 8;
            m_capabilities.supportsExplicitResourceBarriers = true;
            m_capabilities.supportsDefaultQueueFenceSignal = true;
            m_capabilities.supportsExplicitQueueFenceSignal = true;
            m_capabilities.supportsAsyncCompute = true;
            m_capabilities.supportsIndirectDrawCount = true;
            m_capabilities.indexedIndirectExecution.supportsFixedCount = true;
            m_capabilities.indexedIndirectExecution.supportsCountBuffer = true;
            m_capabilities.indexedIndirectExecution.supportsFirstInstance = true;
            m_capabilities.indexedIndirectExecution.requiresExactCommandStride = true;
            m_capabilities.indexedIndirectExecution.indexedCommandSize =
                sizeof(IndirectDrawIndexedCommand);
            m_capabilities.indexedIndirectExecution.minCommandStride =
                sizeof(IndirectDrawIndexedCommand);
            m_capabilities.indexedIndirectExecution.commandStrideAlignment = 4;
            m_capabilities.indexedIndirectExecution.argumentOffsetAlignment = 4;
            m_capabilities.indexedIndirectExecution.countOffsetAlignment = 4;
            m_capabilities.indexedIndirectExecution.maxDrawCount = UINT32_MAX;
            m_capabilities.indexedIndirectExecution.countValueSize = sizeof(uint32);
            m_capabilities.dx12.resourceBindingTier = 2;
            m_capabilities.queueTopology.completionMode =
                RHIQueueCompletionMode::NativeTimeline;
            m_capabilities.queueTopology.logicalQueueDomains = {
                GPUQueueDomain::Graphics,
                GPUQueueDomain::Compute,
                GPUQueueDomain::Copy};
            m_capabilities.queueTopology.activeDomainCount = 3;
        }

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

        RHIPipelineRef CreateGraphicsPipeline(const RHIGraphicsPipelineDesc& desc) override
        {
            createdGraphicsPipelineDebugNames.emplace_back(
                desc.debugName ? desc.debugName : "");
            if (!failGraphicsPipelineDebugName.empty() && desc.debugName &&
                failGraphicsPipelineDebugName == desc.debugName)
            {
                return {};
            }
            return RHIPipelineRef(new FakePipeline(desc));
        }

        RHIPipelineRef CreateComputePipeline(const RHIComputePipelineDesc& desc) override
        {
            return RHIPipelineRef(new FakePipeline(desc));
        }

        RHIAccelerationStructureBuildSizes GetBottomLevelASBuildSizes(const RHIBottomLevelASDesc& desc) override
        {
            ++bottomLevelSizeQueryCount;
            if (!m_capabilities.supportsRaytracing || !ValidateRHIBottomLevelASDesc(desc))
                return {};

            const uint64 geometryCount = static_cast<uint64>(std::max<size_t>(desc.geometries.size(), 1));
            RHIAccelerationStructureBuildSizes sizes;
            sizes.accelerationStructureSize = 4096 * geometryCount;
            sizes.buildScratchSize = 2048 * geometryCount;
            sizes.updateScratchSize = 1024 * geometryCount;
            return sizes;
        }

        RHIAccelerationStructureBuildSizes GetTopLevelASBuildSizes(const RHITopLevelASDesc& desc) override
        {
            ++topLevelSizeQueryCount;
            if (!m_capabilities.supportsRaytracing || !ValidateRHITopLevelASDesc(desc))
                return {};

            RHIAccelerationStructureBuildSizes sizes;
            sizes.accelerationStructureSize = 4096 + 512 * static_cast<uint64>(desc.GetInstanceCount());
            sizes.buildScratchSize = 2048;
            sizes.updateScratchSize = 1024;
            return sizes;
        }

        RHIAccelerationStructureRef CreateAccelerationStructure(const RHIAccelerationStructureDesc& desc) override
        {
            ++createdAccelerationStructureCount;
            createdAccelerationStructureDescs.push_back(desc);
            if (!m_capabilities.supportsRaytracing || desc.size == 0)
                return {};

            return RHIAccelerationStructureRef(new FakeAccelerationStructure(desc));
        }

        RHIPipelineRef CreateRayTracingPipeline(const RHIRayTracingPipelineDesc& desc) override
        {
            ++createdRayTracingPipelineCount;
            if (!m_capabilities.supportsRaytracingPipeline || !ValidateRHIRayTracingPipelineDesc(desc))
                return {};

            return RHIPipelineRef(new FakePipeline(desc));
        }

        RHIShaderTableRef CreateShaderTable(const RHIShaderTableDesc& desc) override
        {
            ++createdShaderTableCount;
            RHIRayTracingValidationResult validation = ValidateRHIShaderTableDesc(desc);
            if (!validation)
                return {};

            return RHIShaderTableRef(new FakeShaderTable(desc));
        }

        RHIDescriptorSetRef CreateDescriptorSet(const RHIDescriptorSetDesc& desc) override
        {
            createdDescriptorSetDescs.push_back(desc);
            auto descriptorSet = RHIDescriptorSetRef(new FakeDescriptorSet(desc));
            createdDescriptorSets.push_back(
                static_cast<FakeDescriptorSet*>(descriptorSet.Get()));
            return descriptorSet;
        }

        RHIQueryPoolRef CreateQueryPool(const RHIQueryPoolDesc& desc) override
        {
            createdQueryPoolDescs.push_back(desc);
            return RHIQueryPoolRef(new FakeQueryPool(desc));
        }

        RHICommandContextRef CreateCommandContext(RHICommandQueueType type) override
        {
            return RHICommandContextRef(new RecordingCommandContext(type));
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
            auto fence = RHIFenceRef(new FakeFence(initialValue, m_autoCompleteFences));
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
        RHIBackendType GetBackendType() const override { return m_capabilities.backendType; }

        void EnableBasicCapabilities()
        {
            m_capabilities.backendType = RHIBackendType::DX12;
            m_capabilities.adapterName = "RenderPassValidation Test Adapter";
            m_capabilities.driverVersion = "RenderPassValidation.Driver.1";
            m_capabilities.supportsComputePipeline = true;
            m_capabilities.supportsDescriptorSets = true;
            m_capabilities.supportsDynamicDescriptorOffsets = true;
            m_capabilities.maxDescriptorSets = 8;
            m_capabilities.supportsExplicitResourceBarriers = true;
            m_capabilities.supportsDefaultQueueFenceSignal = true;
            m_capabilities.supportsExplicitQueueFenceSignal = true;
            m_capabilities.supportsAsyncCompute = true;
            m_capabilities.queueTopology.completionMode =
                RHIQueueCompletionMode::NativeTimeline;
            m_capabilities.queueTopology.logicalQueueDomains = {
                GPUQueueDomain::Graphics,
                GPUQueueDomain::Compute,
                GPUQueueDomain::Copy};
            m_capabilities.queueTopology.activeDomainCount = 3;
            m_capabilities.dx12.resourceBindingTier = 2;
        }

        void EnableRayTracing()
        {
            m_capabilities.backendType = RHIBackendType::DX12;
            m_capabilities.supportsRaytracing = true;
            m_capabilities.supportsRaytracingPipeline = true;
            m_capabilities.supportsAccelerationStructureUpdate = true;
            m_capabilities.supportsAccelerationStructureCompaction = false;
            m_capabilities.maxRayRecursionDepth = 1;
            m_capabilities.shaderGroupHandleSize = 32;
            m_capabilities.shaderGroupHandleAlignment = 32;
            m_capabilities.shaderTableBaseAlignment = 64;
            m_capabilities.supportsDescriptorSets = true;
            m_capabilities.supportsExplicitResourceBarriers = true;
            m_capabilities.maxDescriptorSets = 8;
        }

        void EnableTimestampQueries()
        {
            m_capabilities.supportsTimestampQueries = true;
        }

        void SetFenceAutoComplete(bool enabled)
        {
            m_autoCompleteFences = enabled;
        }

        FakeFence* FindFenceWithSignal(uint64 value) const
        {
            const auto it = std::find_if(
                m_fences.begin(), m_fences.end(),
                [value](const RHIFenceRef& fence)
                {
                    return fence &&
                        static_cast<FakeFence*>(fence.Get())->GetLastSignaledValue() == value;
                });
            return it != m_fences.end()
                ? static_cast<FakeFence*>(it->Get()) : nullptr;
        }

        void EnableCompatibilityWaitIdleCompletion()
        {
            m_capabilities.backendType = RHIBackendType::DX11;
            m_capabilities.queueTopology.completionMode =
                RHIQueueCompletionMode::CompatibilityWaitIdle;
            m_capabilities.queueTopology.logicalQueueDomains = {
                GPUQueueDomain::Graphics,
                GPUQueueDomain::Graphics,
                GPUQueueDomain::Graphics};
            m_capabilities.queueTopology.activeDomainCount = 1;
            m_capabilities.emulatesQueueFences = true;
            m_capabilities.supportsQueueFenceWait = false;
            m_capabilities.supportsAsyncCompute = false;
        }

        bool bufferMapSucceeds = true;
        bool textureViewCreationSucceeds = true;
        bool failDirectionalShadowSRVCreation = false;
        std::string failGraphicsPipelineDebugName;
        bool samplerCreationSucceeds = true;
        uint32 bottomLevelSizeQueryCount = 0;
        uint32 topLevelSizeQueryCount = 0;
        uint32 createdAccelerationStructureCount = 0;
        uint32 createdRayTracingPipelineCount = 0;
        uint32 createdShaderTableCount = 0;
        std::vector<RHIBufferDesc> createdBufferDescs;
        std::vector<FakeBuffer*> createdBuffers;
        std::vector<RHIAccelerationStructureDesc> createdAccelerationStructureDescs;
        std::vector<RHIDescriptorSetDesc> createdDescriptorSetDescs;
        std::vector<FakeDescriptorSet*> createdDescriptorSets;
        std::vector<std::string> createdGraphicsPipelineDebugNames;
        std::vector<RHIQueryPoolDesc> createdQueryPoolDescs;
        std::vector<RHITextureDesc> createdTextureDescs;
        std::vector<RHITextureViewDesc> createdTextureViewDescs;

    private:
        uint64 m_nextFenceValue = 1;
        bool m_autoCompleteFences = true;
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

    class ViewSnapshotPass final : public IRenderPass
    {
    public:
        const char* GetName() const override { return "ViewSnapshotPass"; }
        void Setup(RenderGraphBuilder& builder, const ViewData& view) override
        {
            if (view.colorTarget.IsValid())
            {
                builder.Write(view.colorTarget, RHIResourceState::RenderTarget);
            }
        }
        void Execute(RHICommandContext&, const ViewData& view) override
        {
            observedViewportWidths.push_back(view.viewportWidth);
            observedFrameNumbers.push_back(view.frameNumber);
        }

        std::vector<uint32> observedViewportWidths;
        std::vector<uint64> observedFrameNumbers;
    };

    class SceneRendererOverloadProbePass final : public IRenderPass
    {
    public:
        const char* GetName() const override { return "SceneRendererOverloadProbePass"; }
        int32_t GetPriority() const override { return 175; }

        void Setup(RenderGraphBuilder& builder, const ViewData& view) override
        {
            ++setupCount;
            setupColor = view.colorTarget;
            setupDepth = view.depthTarget;
            setupViewportWidth = view.viewportWidth;
            setupFrameNumber = view.frameNumber;
            (void)builder.ReadWrite(
                view.colorTarget,
                MakeRHIAccessSnapshot(RHIResourceState::RenderTarget,
                                       RHIShaderStage::Pixel));
            (void)builder.Read(view.depthTarget,
                               RHIResourceState::DepthRead,
                               RHIShaderStage::Pixel);
        }

        void Execute(RHICommandContext&, const ViewData& view) override
        {
            ++recordedExecutionCount;
            recordedColor = view.colorTarget;
            recordedDepth = view.depthTarget;
            recordedViewportWidth = view.viewportWidth;
            recordedFrameNumber = view.frameNumber;
            recordedFrameSequence = view.renderFrameExecutionPlan
                ? view.renderFrameExecutionPlan->frameSequence : 0;
        }

        void AddToGraph(RenderGraph&, const ViewData&) override
        {
            ++viewDataOverloadCount;
        }

        void AddToGraph(RenderGraph& graph,
                        const RenderPassRecordContext& context) override
        {
            ++typedOverloadCount;
            typedLegacyAdapter = context.legacyAdapter;
            typedIdentity = context.identity;
            typedContextIdentityValid = context.MatchesTargetGraph(graph) &&
                context.IsFrameIdentityValid() &&
                context.frameSnapshot != nullptr && context.results != nullptr &&
                context.frameSnapshot->identity == context.identity &&
                context.results->identity == context.identity;
            typedColor = context.view.colorTarget;
            typedDepth = context.view.depthTarget;
            typedSnapshotColor = context.frameSnapshot
                ? context.frameSnapshot->view.colorTarget : RGTextureHandle{};
            typedSnapshotDepth = context.frameSnapshot
                ? context.frameSnapshot->view.depthTarget : RGTextureHandle{};
            typedResourcesBelongToGraph =
                HasCurrentGraphProvenance(typedColor, context.identity) &&
                HasCurrentGraphProvenance(typedDepth, context.identity) &&
                graph.GetTextureDesc(typedColor) != nullptr &&
                graph.GetTextureDesc(typedDepth) != nullptr;

            const RenderPassExecutionData execution =
                MakeRenderPassExecutionData(context);
            typedExecutionValid = execution.MatchesTargetGraph(graph) &&
                execution.IsFrameIdentityValid();

            // Exercise the production compatibility adapter with exactly the
            // renderer-issued typed context. Its graph callbacks must retain
            // a value-owned execution snapshot.
            IRenderPass::AddToGraph(graph, context);
        }

        uint32 viewDataOverloadCount = 0;
        uint32 typedOverloadCount = 0;
        uint32 setupCount = 0;
        uint32 recordedExecutionCount = 0;
        bool typedLegacyAdapter = true;
        bool typedContextIdentityValid = false;
        bool typedResourcesBelongToGraph = false;
        bool typedExecutionValid = false;
        RenderPassRecordIdentity typedIdentity{};
        RGTextureHandle typedColor{};
        RGTextureHandle typedDepth{};
        RGTextureHandle typedSnapshotColor{};
        RGTextureHandle typedSnapshotDepth{};
        RGTextureHandle setupColor{};
        RGTextureHandle setupDepth{};
        RGTextureHandle recordedColor{};
        RGTextureHandle recordedDepth{};
        uint32 setupViewportWidth = 0;
        uint64 setupFrameNumber = 0;
        uint32 recordedViewportWidth = 0;
        uint64 recordedFrameNumber = 0;
        uint64 recordedFrameSequence = 0;
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
        PostProcessFrameInputRequirements GetFrameInputRequirements() const override
        {
            return requirements;
        }

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
        PostProcessFrameInputRequirements requirements;

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

    std::unique_ptr<Resource::MeshResource> CreateTwoSubmeshMeshResource(Resource::ResourceId id)
    {
        auto resource = std::make_unique<Resource::MeshResource>();
        resource->SetId(id);
        resource->SetName("RenderPassTwoSubmeshMesh");

        auto mesh = std::make_shared<Mesh>();
        mesh->name = "TwoSubmeshTriangles";

        std::vector<Vec3> positions = {
            {-0.75f, 0.5f, 0.0f},
            {-1.0f, -0.5f, 0.0f},
            {-0.5f, -0.5f, 0.0f},
            {0.75f, 0.5f, 0.0f},
            {0.5f, -0.5f, 0.0f},
            {1.0f, -0.5f, 0.0f}
        };
        std::vector<Vec3> normals(positions.size(), Vec3(0.0f, 0.0f, 1.0f));
        std::vector<Vec2> uvs = {
            {0.5f, 1.0f},
            {0.0f, 0.0f},
            {1.0f, 0.0f},
            {0.5f, 1.0f},
            {0.0f, 0.0f},
            {1.0f, 0.0f}
        };
        std::vector<uint32_t> indices = {0, 1, 2, 3, 4, 5};

        SubMesh firstSubmesh;
        firstSubmesh.indexOffset = 0;
        firstSubmesh.indexCount = 3;
        firstSubmesh.baseVertex = 0;

        SubMesh secondSubmesh;
        secondSubmesh.indexOffset = 3;
        secondSubmesh.indexCount = 3;
        secondSubmesh.baseVertex = 0;

        mesh->SetPositions(positions);
        mesh->SetNormals(normals);
        mesh->SetUVs(uvs);
        mesh->SetIndices(indices);
        mesh->SetSubMeshes({firstSubmesh, secondSubmesh});
        mesh->ComputeBoundingBox();

        resource->SetMesh(mesh);
        return resource;
    }

    RenderObject MakeRenderObject(
        Resource::MeshResource& meshResource,
        const RenderRuntimeTestHarness& gpuResources)
    {
        RenderObject object;
        object.mesh = gpuResources.GetHandle(meshResource.GetId());
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
        item.mesh = RenderResourceHandle{401, 1};
        item.material = RenderResourceHandle{
            static_cast<uint32>(mode) + 1U, 1};
        item.renderMode = mode;
        return item;
    }

    void ConfigureResources(ShadowPass& pass,
                            RenderRuntimeTestHarness& gpuResources,
                            PipelineCache& pipelineCache)
    {
        pass.SetResources(&pipelineCache);
        pass.SetResourceRegistry(&gpuResources.GetRegistry());
    }

    void ConfigureResources(DepthPrepass& pass,
                            RenderRuntimeTestHarness& gpuResources,
                            PipelineCache& pipelineCache)
    {
        pass.SetResources(&pipelineCache);
        pass.SetResourceRegistry(&gpuResources.GetRegistry());
    }

    RenderPassPolicyFacts MakeDirectPlanFacts(
        RenderPassKind pass,
        const MeshPassPacketStream& stream)
    {
        RenderPassPolicyFacts facts;
        facts.pass = pass;
        facts.requested = true;
        facts.supported = true;
        facts.directAllowed = true;
        facts.gpuDrivenAllowed = pass == RenderPassKind::Depth ||
                                 pass == RenderPassKind::Opaque;
        facts.fixedCountIndirectAllowed = true;
        facts.directShaderReadiness = RenderPolicyReadiness::Ready;
        facts.directPipelineReadiness = RenderPolicyReadiness::Ready;
        facts.directResourceReadiness = RenderPolicyReadiness::Ready;
        facts.gpuDrivenShaderReadiness = RenderPolicyReadiness::Ready;
        facts.gpuDrivenPipelineReadiness = RenderPolicyReadiness::Ready;
        facts.gpuDrivenResourceReadiness = RenderPolicyReadiness::Ready;
        facts.inputPacketCount = stream.stats.inputPacketCount;
        facts.relevantPacketCount = stream.stats.relevantPacketCount;
        facts.candidatePacketCount = stream.stats.gpuCandidatePacketCount;
        facts.directPacketCount = stream.stats.directPacketCount;
        facts.skippedPacketCount = stream.stats.skippedPacketCount;
        facts.drawGroupCount = static_cast<uint32>(stream.groups.size());
        facts.workloadBeneficial = true;
        return facts;
    }

    RenderFramePlanCompileResult CompilePlan(
        const SceneMeshPassPreparation& preparation,
        RenderGPUDrivenMode gpuDrivenMode,
        uint64 frameSequence,
        RenderPolicyReadiness directResourceReadiness =
            RenderPolicyReadiness::Ready)
    {
        RenderPolicyResolverInput input;
        input.request.frameSequence = frameSequence;
        input.request.gpuDrivenMode = gpuDrivenMode;
        input.view.rendererAllowsGPUDriven = true;
        input.view.viewAllowsGPUDriven = true;
        input.view.implementationAvailable = true;
        input.view.requestedVisibility = RenderVisibilityMode::GpuFrustum;
        input.view.visibilityShaderReadiness = RenderPolicyReadiness::Ready;
        input.view.visibilityPipelineReadiness = RenderPolicyReadiness::Ready;
        input.view.sharedResourceReadiness = RenderPolicyReadiness::Ready;
        input.view.requiredBindingReadiness = RenderPolicyReadiness::Ready;
        input.capabilities.backend = RHIBackendType::DX12;
        input.capabilities.supportsComputeVisibility = true;
        input.capabilities.supportsDescriptorResourceBindings = true;
        input.capabilities.indexedIndirectExecution.supportsCountBuffer = true;
        input.qualification.backend = RHIBackendType::DX12;
        input.qualification.revision = 1;
        input.qualification.passedGateMask =
            input.qualification.requiredGateMask;
        input.passes = {
            MakeDirectPlanFacts(RenderPassKind::Depth, preparation.depth),
            MakeDirectPlanFacts(RenderPassKind::Opaque, preparation.opaque),
            MakeDirectPlanFacts(RenderPassKind::Shadow, preparation.shadow),
            MakeDirectPlanFacts(RenderPassKind::Transparent,
                                preparation.transparent),
        };
        for (RenderPassPolicyFacts& facts : input.passes)
        {
            facts.directResourceReadiness = directResourceReadiness;
        }
        return CompileRenderFrameExecutionPlan(
            ResolveRenderPolicy(input), preparation);
    }

    RenderFramePlanCompileResult CompileForcedDirectPlan(
        const SceneMeshPassPreparation& preparation,
        uint64 frameSequence = 601)
    {
        return CompilePlan(preparation,
                           RenderGPUDrivenMode::ForceDisabled,
                           frameSequence);
    }

    RenderFramePlanCompileResult CompileForcedGPUPlan(
        const SceneMeshPassPreparation& preparation,
        uint64 frameSequence = 607)
    {
        return CompilePlan(preparation,
                           RenderGPUDrivenMode::ForceEnabled,
                           frameSequence);
    }

    SceneMeshPassPreparation PrepareOpaquePackets(
        std::span<const RenderDrawItem> opaqueItems,
        std::span<const RenderDrawItem> maskedItems)
    {
        SceneMeshPassPreparation preparation;
        OpaqueMeshPassProcessor processor;
        uint32 sourceOrdinal = 0;
        const auto record = [&](const RenderDrawItem& item)
        {
            MeshPassProcessorInput input;
            input.packet = item.packet;
            input.availability.pipeline = MeshPassResourceAvailability::Ready;
            input.availability.geometry = MeshPassResourceAvailability::Ready;
            input.availability.material = MeshPassResourceAvailability::Ready;
            input.sourceOrdinal = sourceOrdinal++;
            preparation.opaque.Record(processor.Process(input));
        };
        for (const RenderDrawItem& item : opaqueItems)
            record(item);
        for (const RenderDrawItem& item : maskedItems)
            record(item);
        preparation.depth.FinalizeGroups();
        preparation.opaque.FinalizeGroups();
        preparation.shadow.FinalizeGroups();
        preparation.transparent.FinalizeGroups();
        return preparation;
    }

    RenderDrawPacket MakeDepthPacket(
        const RenderScene& scene,
        uint32 objectIndex,
        uint32 submeshIndex,
        const MeshGPUBuffers& buffers,
        RenderResourceHandle material,
        RenderMaterialMode materialMode,
        RenderDrawFlags flags)
    {
        RenderDrawPacket packet;
        const RenderObject& object = scene.GetObject(objectIndex);
        packet.objectId = object.entityId;
        packet.primitiveData = objectIndex;
        packet.submeshIndex = submeshIndex;
        packet.pipelineKey.materialVariant =
            materialMode == RenderMaterialMode::Masked
                ? MaterialPipelineVariant::Masked
                : MaterialPipelineVariant::Opaque;
        packet.pipelineKey.topology = MeshUploadPrimitiveTopology::Triangles;
        packet.pipelineKey.skinned =
            (static_cast<uint32>(flags) &
             static_cast<uint32>(RenderDrawFlags::Skinned)) != 0;
        packet.geometryKey.mesh = object.mesh;
        packet.geometryKey.submeshIndex = submeshIndex;
        packet.geometryKey.indexType = MeshUploadIndexType::UInt32;
        packet.materialKey.material = material;
        packet.materialKey.materialMode = materialMode;
        packet.flags = flags;
        if (object.castsShadow)
            packet.flags = packet.flags | RenderDrawFlags::CastsShadow;
        if (object.receivesShadow)
            packet.flags = packet.flags | RenderDrawFlags::ReceivesShadow;
        if (submeshIndex < buffers.submeshes.size())
        {
            const SubmeshGPUInfo& submesh = buffers.submeshes[submeshIndex];
            packet.arguments.indexCount = submesh.indexCount;
            packet.arguments.firstIndex = submesh.indexOffset;
            packet.arguments.vertexOffset = submesh.baseVertex;
        }
        return packet;
    }

    RenderDrawPacket MakeOpaquePacket(
        const RenderScene& scene,
        uint32 objectIndex,
        uint32 submeshIndex,
        const MeshGPUBuffers& buffers,
        RenderResourceHandle material,
        RenderMaterialMode materialMode,
        RenderDrawFlags flags)
    {
        return MakeDepthPacket(scene,
                               objectIndex,
                               submeshIndex,
                               buffers,
                               material,
                               materialMode,
                               flags);
    }

    SceneMeshPassPreparation PrepareDepthPackets(
        std::span<const RenderDrawItem> opaqueItems,
        std::span<const RenderDrawItem> maskedItems)
    {
        SceneMeshPassPreparation preparation;
        DepthMeshPassProcessor processor;
        uint32 sourceOrdinal = 0;
        const auto record = [&](const RenderDrawItem& item)
        {
            MeshPassProcessorInput input;
            input.packet = item.packet;
            input.availability.pipeline = MeshPassResourceAvailability::Ready;
            input.availability.geometry = MeshPassResourceAvailability::Ready;
            input.availability.material = MeshPassResourceAvailability::Ready;
            input.sourceOrdinal = sourceOrdinal++;
            preparation.depth.Record(processor.Process(input));
        };
        for (const RenderDrawItem& item : opaqueItems)
            record(item);
        for (const RenderDrawItem& item : maskedItems)
            record(item);
        preparation.depth.FinalizeGroups();
        preparation.opaque.FinalizeGroups();
        preparation.shadow.FinalizeGroups();
        preparation.transparent.FinalizeGroups();
        return preparation;
    }

    RenderFrameExecutionReport MakeExecutionReport(
        const RenderFrameExecutionPlan& plan)
    {
        RenderFrameExecutionReport frameReport;
        frameReport.frameSequence = plan.frameSequence;
        for (const RenderPassExecutionPlan& passPlan : plan.passes)
        {
            RenderPassExecutionReport report;
            report.pass = passPlan.pass;
            report.executedVisibility = passPlan.visibility;
            report.gpuDrivenLane.packetRange = passPlan.gpuEligiblePackets;
            report.gpuDrivenLane.submission = passPlan.preferredSubmission;
            report.directLane.packetRange = passPlan.directPackets;
            report.directLane.submission = passPlan.fallbackSubmission;
            frameReport.passes.push_back(report);
        }
        return frameReport;
    }

    const RenderPassExecutionReport* FindPassExecutionReport(
        const RenderFrameExecutionReport& report,
        RenderPassKind pass)
    {
        const auto found = std::find_if(
            report.passes.begin(),
            report.passes.end(),
            [pass](const RenderPassExecutionReport& candidate)
            {
                return candidate.pass == pass;
            });
        return found == report.passes.end() ? nullptr : &*found;
    }

    void ConfigureResources(RayTracedShadowPass& pass,
                            RenderRuntimeTestHarness& gpuResources,
                            PipelineCache& pipelineCache,
                            ResourceViewCache& viewCache)
    {
        pass.SetResources(&pipelineCache, &viewCache);
        pass.SetResourceRegistry(&gpuResources.GetRegistry());
    }

    void ConfigureResources(RayTracedReflectionPass& pass,
                            RenderRuntimeTestHarness& gpuResources,
                            PipelineCache& pipelineCache,
                            ResourceViewCache& viewCache)
    {
        pass.SetResources(&pipelineCache, &viewCache);
        pass.SetResourceRegistry(&gpuResources.GetRegistry());
    }

    void ConfigureResources(ObjectVelocityPass& pass,
                            RenderRuntimeTestHarness& gpuResources,
                            PipelineCache& pipelineCache,
                            ResourceViewCache& viewCache,
                            MaterialSystem& materialSystem)
    {
        pass.SetResources(&pipelineCache, &viewCache, &materialSystem);
        pass.SetResourceRegistry(&gpuResources.GetRegistry());
    }

    void ConfigureResources(OpaquePass& pass,
                            RenderRuntimeTestHarness& gpuResources,
                            PipelineCache& pipelineCache,
                            MaterialSystem& materialSystem)
    {
        pass.SetResources(&pipelineCache, &materialSystem);
        pass.SetResourceRegistry(&gpuResources.GetRegistry());
    }

    void ConfigureResources(TransparentPass& pass,
                            RenderRuntimeTestHarness& gpuResources,
                            PipelineCache& pipelineCache,
                            MaterialSystem& materialSystem)
    {
        pass.SetResources(&pipelineCache, &materialSystem);
        pass.SetResourceRegistry(&gpuResources.GetRegistry());
    }

    void ConfigurePrimaryDirectionalLightRecord(
        RenderPassRecordContext& context,
        const Vec3& direction,
        const Vec3& color,
        float32 intensity,
        bool castsShadow = true)
    {
        context.primaryDirectionalLight.selected = true;
        context.primaryDirectionalLight.castsShadow = castsShadow;
        context.primaryDirectionalLight.direction = direction;
        context.primaryDirectionalLight.color = color;
        context.primaryDirectionalLight.intensity = intensity;
        if (context.results)
        {
            context.frameSnapshot = MakeRenderPassFrameSnapshot(
                context, *context.results);
        }
    }

    RenderPassRecordContext MakeTransparentRecordContext(
        RenderGraph& graph,
        ViewData view,
        const RenderScene& scene,
        const std::vector<RenderDrawItem>& transparentDrawItems,
        uint64 frameSequence,
        uint64 recordEpoch,
        RenderSubmissionResourceBatch* batch = nullptr)
    {
        RenderPassRecordContext context;
        context.view = view;
        context.view.renderGraph = &graph;
        context.view.submissionResourceBatch = batch;
        context.executionPlan = context.view.renderFrameExecutionPlan;
        context.meshPassPreparation = context.view.meshPassPreparation;
        context.visibility = context.view.renderVisibility;
        context.executionReport = context.view.renderFrameExecutionReport;
        context.identity.graph = &graph;
        context.identity.graphIdentity = graph.GetGraphIdentity();
        context.identity.graphRecordingGeneration = graph.GetRecordingGeneration();
        context.identity.frameSequence = frameSequence;
        context.identity.viewOrdinal = 0;
        context.identity.recordEpoch = recordEpoch;
        context.renderScene = &scene;
        context.transparentDrawItems = &transparentDrawItems;
        context.results = std::make_shared<RenderPassRecordResults>();
        context.frameSnapshot = MakeRenderPassFrameSnapshot(context, *context.results);
        return context;
    }

    RenderPassRecordContext MakeSkyboxRecordContext(
        RenderGraph& graph,
        ViewData view,
        const RenderScene& scene,
        const RenderSkySnapshot& sky,
        uint64 frameSequence,
        uint64 recordEpoch,
        RenderSubmissionResourceBatch* batch = nullptr)
    {
        RenderPassRecordContext context;
        context.view = view;
        context.view.renderGraph = &graph;
        context.view.submissionResourceBatch = batch;
        context.executionPlan = context.view.renderFrameExecutionPlan;
        context.meshPassPreparation = context.view.meshPassPreparation;
        context.visibility = context.view.renderVisibility;
        context.executionReport = context.view.renderFrameExecutionReport;
        context.identity.graph = &graph;
        context.identity.graphIdentity = graph.GetGraphIdentity();
        context.identity.graphRecordingGeneration = graph.GetRecordingGeneration();
        context.identity.frameSequence = frameSequence;
        context.identity.viewOrdinal = 0;
        context.identity.recordEpoch = recordEpoch;
        context.renderScene = &scene;
        context.results = std::make_shared<RenderPassRecordResults>();
        context.frameSnapshot = MakeRenderPassFrameSnapshot(context, *context.results);

        // Tests deliberately alter only the value-owned sky while preserving
        // the internal self-references that make a copied frame snapshot valid.
        auto frameSnapshot = std::make_shared<RenderPassFrameSnapshot>(
            *context.frameSnapshot);
        frameSnapshot->sky = sky;
        frameSnapshot->view.renderGraph = &graph;
        frameSnapshot->view.renderFrameExecutionPlan =
            &frameSnapshot->executionPlan;
        frameSnapshot->view.meshPassPreparation =
            &frameSnapshot->meshPassPreparation;
        frameSnapshot->view.renderVisibility = &frameSnapshot->visibility;
        frameSnapshot->view.renderFrameExecutionReport =
            &context.results->executionReport;
        context.frameSnapshot = std::move(frameSnapshot);
        return context;
    }

    RenderPassRecordContext MakeMainSceneRecordContext(
        RenderGraph& graph,
        ViewData frameView,
        const RenderScene& scene,
        const std::vector<RenderDrawItem>& opaqueDrawItems,
        const std::vector<RenderDrawItem>& maskedDrawItems,
        const RenderFrameExecutionPlan& executionPlan,
        const SceneMeshPassPreparation& preparation,
        RenderFrameExecutionReport& executionReport,
        uint64 recordEpoch,
        RenderSubmissionResourceBatch* batch = nullptr)
    {
        RenderPassRecordContext context;
        context.view = frameView;
        context.view.renderGraph = &graph;
        context.view.viewCache = frameView.viewCache;
        context.view.submissionResourceBatch = batch;
        context.view.renderFrameExecutionPlan = &executionPlan;
        context.view.meshPassPreparation = &preparation;
        context.view.renderFrameExecutionReport = &executionReport;
        context.identity.graph = &graph;
        context.identity.graphIdentity = graph.GetGraphIdentity();
        context.identity.graphRecordingGeneration = graph.GetRecordingGeneration();
        context.identity.frameSequence = executionPlan.frameSequence;
        context.identity.viewOrdinal = executionPlan.viewOrdinal;
        context.identity.recordEpoch = recordEpoch;
        context.executionPlan = &executionPlan;
        context.meshPassPreparation = &preparation;
        context.visibility = frameView.renderVisibility;
        context.executionReport = &executionReport;
        context.renderScene = &scene;
        context.opaqueDrawItems = &opaqueDrawItems;
        context.maskedDrawItems = &maskedDrawItems;
        context.directionalShadow.identity = context.identity;
        context.rayTracedShadow.identity = context.identity;
        context.results = std::make_shared<RenderPassRecordResults>();
        RenderVisibilityResult synthesizedVisibility;
        if (context.visibility == nullptr)
        {
            synthesizedVisibility.structurallyValid = true;
            const auto markVisible = [&synthesizedVisibility](
                                         RenderPassKind pass,
                                         const MeshPassPacketStream& packets)
            {
                RenderVisibilityPassResult& passVisibility =
                    synthesizedVisibility.passes[static_cast<size_t>(pass)];
                passVisibility.pass = pass;
                passVisibility.sourcePacketKnown.assign(
                    packets.packets.size(), 1u);
                passVisibility.cpuVisibleBySourcePacket.assign(
                    packets.packets.size(), 1u);
            };
            markVisible(RenderPassKind::Depth, preparation.depth);
            markVisible(RenderPassKind::Opaque, preparation.opaque);
            markVisible(RenderPassKind::Shadow, preparation.shadow);
            markVisible(RenderPassKind::Transparent, preparation.transparent);
            context.visibility = &synthesizedVisibility;
            context.view.renderVisibility = &synthesizedVisibility;
        }
        context.frameSnapshot = MakeRenderPassFrameSnapshot(
            context, *context.results);
        context.visibility = &context.frameSnapshot->visibility;
        context.view.renderVisibility = &context.frameSnapshot->visibility;
        return context;
    }

    RenderPassGPUDrivenInputs MakeMainSceneGPUDrivenInputs(
        RenderGraph& graph,
        const RenderPassRecordIdentity& identity,
        GPUCulling& culling)
    {
        RenderPassGPUDrivenInputs inputs;
        inputs.identity = identity;
        inputs.recordedState = culling.SealForGraph(GPUCullingRecordingIdentity{
            identity.graphIdentity,
            identity.graphRecordingGeneration,
            identity.frameSequence,
            identity.viewOrdinal,
            identity.recordEpoch});
        if (!inputs.recordedState || !inputs.recordedState->IsValid())
        {
            return {};
        }

        const GPUCulling& recorded = inputs.recordedState->GetCulling();
        const GPUCullingAccessSnapshots& access =
            inputs.recordedState->GetAccessSnapshots();
        inputs.identity = identity;
        inputs.instances = graph.ImportBuffer(
            recorded.GetInstanceBuffer(), access.instances);
        inputs.instanceIndices = graph.ImportBuffer(
            recorded.GetInstanceIndexBuffer(), access.instanceIndices);
        inputs.indirectDraws = graph.ImportBuffer(
            recorded.GetIndirectBuffer(), access.indirectDraws);
        inputs.drawCount = graph.ImportBuffer(
            recorded.GetDrawCountBuffer(), access.drawCount);
        return inputs;
    }

    RenderPassGPUDrivenInputs MakeSyntheticGPUSceneRasterInputs(
        RenderGraph& graph,
        FakeDevice& device,
        PipelineCache& pipelineCache,
        RenderPassGPUDrivenInputs inputs)
    {
        if (!inputs.recordedState || !inputs.recordedState->IsValid())
        {
            return {};
        }

        const auto makeStructuredBuffer = [&device](
                                              uint32 capacity,
                                              uint32 stride,
                                              const char* debugName)
        {
            RHIBufferDesc desc;
            desc.size = static_cast<uint64>(capacity) * stride;
            desc.usage = RHIBufferUsage::Structured |
                RHIBufferUsage::ShaderResource;
            desc.memoryType = RHIMemoryType::Upload;
            desc.stride = stride;
            desc.debugName = debugName;
            return device.CreateBuffer(desc);
        };

        constexpr uint32 capacity = 4u;
        constexpr uint64 leaseVersion = 91u;
        GPUSceneRasterResourceSnapshot resources;
        resources.m_candidateCount =
            inputs.recordedState->GetCulling().GetInstanceCount();
        resources.m_candidateCapacity = capacity;
        resources.m_primitiveCapacity = capacity;
        resources.m_transformCapacity = capacity;
        resources.m_leaseVersion = leaseVersion;
        resources.m_exactLeaseVersion = leaseVersion;
        resources.m_candidates = makeStructuredBuffer(
            capacity, sizeof(GPUSceneCullingCandidate),
            "SyntheticGPUSceneRaster.Candidates");
        resources.m_primitives = makeStructuredBuffer(
            capacity, sizeof(GPUScenePrimitiveRow),
            "SyntheticGPUSceneRaster.Primitives");
        resources.m_transforms = makeStructuredBuffer(
            capacity, sizeof(GPUSceneTransformRow),
            "SyntheticGPUSceneRaster.Transforms");
        if (!resources.IsValid())
        {
            return {};
        }

        GPUSceneRasterBindingSnapshot binding;
        ObjectConstants objectConstants{};
        if (!pipelineCache.CreateGPUSceneRasterBindingSnapshot(
                resources, objectConstants, binding))
        {
            return {};
        }

        inputs.instances = {};
        inputs.gpuSceneCandidates = graph.ImportBuffer(
            resources.GetCandidates(), RHIResourceState::ShaderResource);
        inputs.gpuScenePrimitives = graph.ImportBuffer(
            resources.GetPrimitives(), RHIResourceState::ShaderResource);
        inputs.gpuSceneTransforms = graph.ImportBuffer(
            resources.GetTransforms(), RHIResourceState::ShaderResource);
        inputs.gpuSceneRasterBinding =
            std::make_shared<GPUSceneRasterBindingSnapshot>(std::move(binding));
        inputs.gpuSceneRecordingFailure =
            std::make_shared<std::atomic_bool>(false);
        inputs.gpuSceneLeaseVersion = leaseVersion;
        inputs.gpuSceneRasterEnabled = true;
        return inputs;
    }

    template <typename Pass>
    void RecordAndExecuteMainScenePass(
        RenderGraph& graph,
        Pass& pass,
        const RenderPassRecordContext& context,
        RecordingCommandContext& commandContext)
    {
        pass.AddToGraph(graph, context);
        graph.Compile();
        EXPECT_TRUE(graph.GetCompileStats().compileValid);
        graph.Execute(commandContext);
        pass.PublishRecordResults(context.results, context.identity);
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

    Resource::TextureHandle CreateCubemapTextureResource(Resource::ResourceId id)
    {
        auto* texture = new Resource::TextureResource();
        texture->SetId(id);
        texture->SetName("RenderPassCubemap");

        Resource::TextureMetadata metadata;
        metadata.width = 1;
        metadata.height = 1;
        metadata.arrayLayers = 6;
        metadata.format = Resource::TextureFormat::RGBA8;
        metadata.isCubemap = true;
        metadata.isSRGB = false;
        texture->SetData(std::vector<uint8>(24, 255), metadata);
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

    bool IsAlignedToTexel(float value, float texelSize, float epsilon = 0.0001f)
    {
        if (texelSize <= 0.0f)
            return false;

        const float rounded = std::round(value / texelSize) * texelSize;
        return std::abs(value - rounded) <= epsilon;
    }

    Vec2 ProjectShadowUV(const ShadowCascade& cascade, const Vec3& worldPosition)
    {
        const Vec4 clipPosition = cascade.viewProjection * Vec4(worldPosition, 1.0f);
        if (std::abs(clipPosition.w) <= 0.000001f)
        {
            return Vec2(0.0f, 0.0f);
        }

        const Vec2 ndc(clipPosition.x / clipPosition.w, clipPosition.y / clipPosition.w);
        return Vec2(ndc.x * 0.5f + 0.5f, ndc.y * -0.5f + 0.5f);
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

    FakeBuffer* FindMutableCreatedBuffer(FakeDevice& device,
                                         const char* debugName)
    {
        for (size_t i = 0;
             i < device.createdBufferDescs.size() &&
             i < device.createdBuffers.size();
             ++i)
        {
            const char* name = device.createdBufferDescs[i].debugName;
            if (name && std::string(name) == debugName)
            {
                return device.createdBuffers[i];
            }
        }
        return nullptr;
    }

    void ExpectPipelineDebugName(RHIPipeline* pipeline, const char* expectedName)
    {
        ASSERT_NE(pipeline, nullptr);
        const auto* fakePipeline = dynamic_cast<const FakePipeline*>(pipeline);
        ASSERT_NE(fakePipeline, nullptr);
        EXPECT_EQ(fakePipeline->debugName, expectedName);
    }


    void ExpectCommandBefore(const RecordingCommandContext& ctx, const std::string& before, const std::string& after)
    {
        const auto beforeIt = std::find(ctx.callSequence.begin(), ctx.callSequence.end(), before);
        const auto afterIt = std::find(ctx.callSequence.begin(), ctx.callSequence.end(), after);
        ASSERT_NE(beforeIt, ctx.callSequence.end()) << before;
        ASSERT_NE(afterIt, ctx.callSequence.end()) << after;
        EXPECT_LT(std::distance(ctx.callSequence.begin(), beforeIt),
                  std::distance(ctx.callSequence.begin(), afterIt));
    }

    const SceneRenderFeatureCapability* FindRenderFeature(
        const SceneRenderFeatureReport& report,
        SceneRenderFeature feature)
    {
        const auto it = std::find_if(
            report.features.begin(),
            report.features.end(),
            [feature](const SceneRenderFeatureCapability& candidate)
            {
                return candidate.feature == feature;
            });
        return it != report.features.end() ? &(*it) : nullptr;
    }

    void PrepareRayTracingSceneForSingleObject(FakeDevice& device,
                                               RenderRuntimeTestHarness& gpuResources,
                                               const RenderScene& scene,
                                               RayTracingSceneManager& sceneManager)
    {
        std::vector<uint32_t> visibleObjectIndices{0};
        RayTracingSceneBuildPlan plan = BuildRayTracingSceneBuildPlan(
            scene, visibleObjectIndices, gpuResources.GetRegistry());
        ASSERT_TRUE(plan.HasWork());

        sceneManager.Initialize(&device);
        ASSERT_TRUE(sceneManager.IsSupported());
        ASSERT_TRUE(sceneManager.Prepare(plan)) << sceneManager.GetStats().fallbackReason;

        RecordingCommandContext buildCtx;
        sceneManager.RecordBuildCommands(buildCtx);
        EXPECT_GE(buildCtx.buildBottomLevelASCount, 1u);
        EXPECT_EQ(buildCtx.buildTopLevelASCount, 1u);
        EXPECT_NE(sceneManager.GetTopLevelAS(), nullptr);
        EXPECT_NE(sceneManager.GetInstanceMaterialMetadataBuffer(), nullptr);
        EXPECT_NE(sceneManager.GetInstanceAlphaMetadataBuffer(), nullptr);
    }

    void ExpectRayTracedShadowRecordGateNoWork(
        FakeDevice& device,
        ResourceViewCache& viewCache,
        RayTracedShadowPass& pass,
        const char* caseName,
        bool featureEnabled,
        bool selected,
        bool castsShadow,
        uint64 frameNumber)
    {
        SCOPED_TRACE(caseName);
        RenderGraph graph;
        graph.SetDevice(&device);
        RHITextureRef depthTexture = device.CreateTexture(RHITextureDesc::DepthStencil(
            32, 32, PipelineCache::GetDefaultDepthStencilFormat()));
        ASSERT_TRUE(depthTexture);
        ViewData frameView;
        frameView.renderGraph = &graph;
        frameView.viewCache = &viewCache;
        frameView.depthTarget = graph.ImportTexture(
            depthTexture.Get(), RHIResourceState::DepthRead);
        frameView.viewportWidth = 32;
        frameView.viewportHeight = 32;
        frameView.nearPlane = 0.1f;
        frameView.farPlane = 100.0f;
        frameView.frameNumber = frameNumber;

        pass.SetEnabled(featureEnabled);
        RenderPassRecordContext context = MakeRenderPassRecordContext(graph, frameView);
        context.legacyAdapter = false;
        context.primaryDirectionalLight.selected = selected;
        context.primaryDirectionalLight.castsShadow = castsShadow;
        context.primaryDirectionalLight.direction = Vec3{0.2f, -0.8f, 0.5f};
        context.primaryDirectionalLight.intensity = 3.0f;
        context.results = std::make_shared<RenderPassRecordResults>();
        context.frameSnapshot = MakeRenderPassFrameSnapshot(context, *context.results);
        const size_t texturesBefore = device.createdTextureDescs.size();
        const size_t buffersBefore = device.createdBufferDescs.size();
        const size_t descriptorSetsBefore = device.createdDescriptorSetDescs.size();

        pass.AddToGraph(graph, context);

        ASSERT_TRUE(context.results->rayTracedShadowOutput.executionState);
        EXPECT_EQ(context.results->rayTracedShadowOutput.identity, context.identity);
        EXPECT_TRUE(context.results->rayTracedShadowOutput.IsCompatibleWith(
            context.identity));
        EXPECT_FALSE(context.results->rayTracedShadowOutput.enabled);
        EXPECT_FALSE(context.results->rayTracedShadowOutput.shadowMask.IsValid());
        EXPECT_EQ(context.results->rayTracedShadowStats.requested, featureEnabled);
        EXPECT_FALSE(context.results->rayTracedShadowStats.supported);
        EXPECT_FALSE(context.results->rayTracedShadowStats.historyAvailable);
        EXPECT_FALSE(context.results->rayTracedShadowStats.historyRecreated);
        EXPECT_FALSE(context.results->rayTracedShadowStats.outputDeclared);
        // The authoritative record gate precedes history allocation/reservation,
        // per-frame resources, graph registration, and dispatch.
        EXPECT_EQ(device.createdTextureDescs.size(), texturesBefore);
        EXPECT_EQ(device.createdBufferDescs.size(), buffersBefore);
        EXPECT_EQ(device.createdDescriptorSetDescs.size(), descriptorSetsBefore);

        graph.Compile();
        ASSERT_TRUE(graph.GetCompileStats().compileValid);
        EXPECT_EQ(graph.GetCompileStats().totalPasses, 0u);
        RecordingCommandContext commands;
        graph.Execute(commands);
        EXPECT_EQ(commands.dispatchRaysCount, 0u);
        EXPECT_EQ(device.createdTextureDescs.size(), texturesBefore);
        EXPECT_EQ(device.createdBufferDescs.size(), buffersBefore);
        EXPECT_EQ(device.createdDescriptorSetDescs.size(), descriptorSetsBefore);
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

        void Initialize(bool bufferMapSucceeds = true,
                        bool rayTracingSupported = false,
                        bool reverseZ = false)
        {
            const fs::path shaderDir = FindShaderDirectory();
            if (shaderDir.empty())
            {
                GTEST_SKIP() << "Render/Shaders directory not found";
            }

            device.bufferMapSucceeds = bufferMapSucceeds;
            if (rayTracingSupported)
            {
                device.EnableRayTracing();
            }

            PipelineCacheConfig pipelineCacheConfig = pipelineCache.GetConfig();
            pipelineCacheConfig.reverseZ = reverseZ;
            pipelineCache.SetConfig(pipelineCacheConfig);
            ASSERT_TRUE(pipelineCache.Initialize(&device, shaderDir.string())) << pipelineCache.GetLastError();

            ASSERT_TRUE(gpuResources.Initialize(&device));
            viewCache.Initialize(&device);
            ASSERT_TRUE(materialSystem.Initialize(
                &device,
                pipelineCache.GetMaterialSetLayout(),
                &gpuResources.GetRegistry()));

            meshResource = CreateMeshResource(401);
            gpuResources.UploadImmediate(meshResource.get());
            ASSERT_TRUE(gpuResources.IsGPUReady(meshResource->GetId()));

            scene.AddObject(MakeRenderObject(*meshResource, gpuResources));

            colorTexture = device.CreateTexture(RHITextureDesc::Texture2D(64, 64, RHIFormat::RGBA8_UNORM));
            ASSERT_TRUE(colorTexture);
            colorView = device.CreateTextureView(colorTexture.Get());
            ASSERT_TRUE(colorView);
        }

        RenderDrawItem MakeDrawItem(MaterialRenderMode mode) const
        {
            RenderDrawItem item;
            item.objectIndex = 0;
            item.submeshIndex = 0;
            item.mesh = meshResource
                ? gpuResources.GetHandle(meshResource->GetId())
                : RenderResourceHandle{};
            item.material = RenderResourceHandle{
                static_cast<uint32>(mode) + 1U, 1};
            item.renderMode = mode;
            return item;
        }

        void ExecuteTypedDepthRecording(
            DepthPrepass& pass,
            ViewData frameView,
            const std::vector<RenderDrawItem>& opaqueDrawItems,
            const std::vector<RenderDrawItem>& maskedDrawItems,
            const RenderFrameExecutionPlan& executionPlan,
            const SceneMeshPassPreparation& preparation,
            RenderFrameExecutionReport& executionReport,
            RHITextureView* depthAttachment,
            GPUCulling* culling,
            RecordingCommandContext& commandContext,
            RenderSubmissionResourceBatch* batch = nullptr)
        {
            RenderGraph graph;
            graph.SetDevice(&device);
            frameView.viewCache = &viewCache;
            if (depthAttachment != nullptr && depthAttachment->GetTexture() != nullptr)
            {
                frameView.depthTarget = graph.ImportTexture(
                    depthAttachment->GetTexture(), RHIResourceState::DepthWrite);
            }
            RenderPassRecordContext context = MakeMainSceneRecordContext(
                graph, frameView, scene, opaqueDrawItems, maskedDrawItems,
                executionPlan, preparation, executionReport,
                executionPlan.frameSequence, batch);
            if (culling != nullptr)
            {
                context.depthGPUDriven = MakeMainSceneGPUDrivenInputs(
                    graph, context.identity, *culling);
            }
            pass.AddToGraph(graph, context);
            graph.Compile();
            EXPECT_TRUE(graph.GetCompileStats().compileValid);
            graph.Execute(commandContext);
            pass.PublishRecordResults(context.results, context.identity);
            executionReport = context.results->executionReport;
        }

        void ExecuteTypedOpaqueRecording(
            OpaquePass& pass,
            ViewData frameView,
            const std::vector<RenderDrawItem>& opaqueDrawItems,
            const std::vector<RenderDrawItem>& maskedDrawItems,
            const RenderFrameExecutionPlan& executionPlan,
            const SceneMeshPassPreparation& preparation,
            RenderFrameExecutionReport& executionReport,
            RHITextureView* colorAttachment,
            RHITextureView* depthAttachment,
            GPUCulling* culling,
            RecordingCommandContext& commandContext,
            const DirectionalShadowRecordOutput& directionalShadow = {},
            const RayTracedShadowRecordOutput& rayTracedShadow = {},
            RenderSubmissionResourceBatch* batch = nullptr)
        {
            RenderGraph graph;
            graph.SetDevice(&device);
            frameView.viewCache = &viewCache;
            if (colorAttachment != nullptr && colorAttachment->GetTexture() != nullptr)
            {
                frameView.colorTarget = graph.ImportTexture(
                    colorAttachment->GetTexture(), RHIResourceState::RenderTarget);
            }
            if (depthAttachment != nullptr && depthAttachment->GetTexture() != nullptr)
            {
                frameView.depthTarget = graph.ImportTexture(
                    depthAttachment->GetTexture(), RHIResourceState::DepthWrite);
            }
            RenderPassRecordContext context = MakeMainSceneRecordContext(
                graph, frameView, scene, opaqueDrawItems, maskedDrawItems,
                executionPlan, preparation, executionReport,
                executionPlan.frameSequence, batch);
            context.directionalShadow = directionalShadow;
            context.directionalShadow.identity = context.identity;
            context.rayTracedShadow = rayTracedShadow;
            context.rayTracedShadow.identity = context.identity;
            if (rayTracedShadow.executionState != nullptr)
            {
                context.rayTracedShadow.executionState->identity = context.identity;
            }
            if (culling != nullptr)
            {
                context.opaqueGPUDriven = MakeMainSceneGPUDrivenInputs(
                    graph, context.identity, *culling);
            }
            context.frameSnapshot = MakeRenderPassFrameSnapshot(
                context, *context.results);
            pass.AddToGraph(graph, context);
            graph.Compile();
            EXPECT_TRUE(graph.GetCompileStats().compileValid);
            graph.Execute(commandContext);
            pass.PublishRecordResults(context.results, context.identity);
            executionReport = context.results->executionReport;
        }

        void ExecuteTypedShadowRecording(
            ShadowPass& pass,
            ViewData frameView,
            const std::vector<RenderDrawItem>& opaqueDrawItems,
            const std::vector<RenderDrawItem>& maskedDrawItems,
            const RenderFrameExecutionPlan& executionPlan,
            const SceneMeshPassPreparation& preparation,
            RenderFrameExecutionReport& executionReport,
            RecordingCommandContext& commandContext,
            const PrimaryDirectionalLightRecordInput& primaryLight,
            RenderSubmissionResourceBatch* batch = nullptr)
        {
            RenderGraph graph;
            graph.SetDevice(&device);
            frameView.viewCache = &viewCache;
            RenderPassRecordContext context = MakeMainSceneRecordContext(
                graph, frameView, scene, opaqueDrawItems, maskedDrawItems,
                executionPlan, preparation, executionReport,
                executionPlan.frameSequence, batch);
            context.primaryDirectionalLight = primaryLight;
            context.frameSnapshot = MakeRenderPassFrameSnapshot(
                context, *context.results);
            pass.AddToGraph(graph, context);
            graph.Compile();
            EXPECT_TRUE(graph.GetCompileStats().compileValid);
            graph.Execute(commandContext);
            pass.PublishRecordResults(context.results, context.identity);
            executionReport = context.results->executionReport;
        }

        std::shared_ptr<RenderPassRecordResults> AddTypedShadowRecording(
            RenderGraph& graph,
            ShadowPass& pass,
            ViewData frameView,
            const std::vector<RenderDrawItem>& opaqueDrawItems,
            const std::vector<RenderDrawItem>& maskedDrawItems,
            const RenderFrameExecutionPlan& executionPlan,
            const SceneMeshPassPreparation& preparation,
            RenderFrameExecutionReport& executionReport,
            const PrimaryDirectionalLightRecordInput& primaryLight,
            RenderSubmissionResourceBatch* batch = nullptr)
        {
            frameView.viewCache = &viewCache;
            RenderPassRecordContext context = MakeMainSceneRecordContext(
                graph, frameView, scene, opaqueDrawItems, maskedDrawItems,
                executionPlan, preparation, executionReport,
                executionPlan.frameSequence, batch);
            context.primaryDirectionalLight = primaryLight;
            context.frameSnapshot = MakeRenderPassFrameSnapshot(
                context, *context.results);
            pass.AddToGraph(graph, context);
            return context.results;
        }

        PrimaryDirectionalLightRecordInput MakeShadowPrimaryLight() const
        {
            PrimaryDirectionalLightRecordInput primaryLight;
            primaryLight.selected = true;
            primaryLight.castsShadow = true;
            primaryLight.direction = Vec3(-0.4f, -1.0f, -0.25f);
            primaryLight.color = Vec3(1.0f, 1.0f, 1.0f);
            primaryLight.intensity = 1.0f;
            return primaryLight;
        }

        void TearDown() override
        {
            materialSystem.Shutdown();
            viewCache.Shutdown();
            gpuResources.Shutdown();
            pipelineCache.Shutdown();
        }

        FakeDevice device;
        PipelineCache pipelineCache{
            RVX::Tests::CreateDeterministicShaderCompiler};
        RenderRuntimeTestHarness gpuResources;
        ResourceViewCache viewCache;
        MaterialSystem materialSystem;
        RenderScene scene;
        std::unique_ptr<Resource::MeshResource> meshResource;
        RHITextureRef colorTexture;
        RHITextureViewRef colorView;
        ViewData view;
    };
} // namespace

TEST_F(RenderPassValidationFixture,
     PrivateBindingSnapshotUsesSealedTablesAndRetainsIndependentPipelineState)
{
    const fs::path shaderDir = FindShaderDirectory();
    if (shaderDir.empty())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    FakeDevice testDevice;
    PipelineCache cache{RVX::Tests::CreateDeterministicShaderCompiler};
    ASSERT_TRUE(cache.Initialize(&testDevice, shaderDir.string())) << cache.GetLastError();
    RHIDescriptorSet* const directObjectSet = cache.GetObjectDescriptorSet();
    RHIDescriptorSetLayout* const directObjectLayout = cache.GetObjectSetLayout();
    ASSERT_NE(nullptr, directObjectSet);
    ASSERT_NE(nullptr, directObjectLayout);

    const auto makeStructuredBuffer = [&testDevice](uint32 capacity, uint32 stride,
                                                const char* debugName)
    {
        RHIBufferDesc desc;
        desc.size = static_cast<uint64>(capacity) * stride;
        desc.usage = RHIBufferUsage::Structured | RHIBufferUsage::ShaderResource;
        desc.memoryType = RHIMemoryType::Upload;
        desc.stride = stride;
        desc.debugName = debugName;
        return testDevice.CreateBuffer(desc);
    };

    // The test has private test access only to create a representative sealed
    // record. Production callers receive this state exclusively from GPUCulling.
    GPUSceneRasterResourceSnapshot resources;
    resources.m_candidateCount = 2u;
    resources.m_candidateCapacity = 4u;
    resources.m_primitiveCapacity = 7u;
    resources.m_transformCapacity = 9u;
    resources.m_leaseVersion = 71u;
    resources.m_exactLeaseVersion = 71u;
    resources.m_candidates = makeStructuredBuffer(
        resources.m_candidateCapacity, sizeof(GPUSceneCullingCandidate),
        "GPUSceneRasterBindingValidation.Candidates");
    resources.m_primitives = makeStructuredBuffer(
        resources.m_primitiveCapacity, sizeof(GPUScenePrimitiveRow),
        "GPUSceneRasterBindingValidation.Primitives");
    resources.m_transforms = makeStructuredBuffer(
        resources.m_transformCapacity, sizeof(GPUSceneTransformRow),
        "GPUSceneRasterBindingValidation.Transforms");
    ASSERT_TRUE(resources.IsValid());

    ObjectConstants objectConstants{};
    objectConstants.world[3] = Vec4(5.0f, 6.0f, 7.0f, 1.0f);
    objectConstants.normalMatrix[0][0] = 0.5f;
    objectConstants.previousWorldViewProjection[1][2] = 3.0f;
    objectConstants.objectVelocityParams = Vec4(1.0f, 0.0f, 1.0f, 0.0f);
    objectConstants.skinningParams = Vec4(4.0f, 3.0f, 2.0f, 1.0f);
    objectConstants.skinningMatrices[3][2][1] = 8.0f;

    FakeBuffer* const candidateBuffer =
        static_cast<FakeBuffer*>(resources.m_candidates.Get());
    ASSERT_NE(nullptr, candidateBuffer);
    const uint32 candidateStride = candidateBuffer->GetStride();
    const uint64 candidateSize = candidateBuffer->GetSize();
    const size_t descriptorCountBeforeFailure = testDevice.createdDescriptorSetDescs.size();
    GPUSceneRasterBindingSnapshot rejected;
    candidateBuffer->SetReportedStride(candidateStride + 4u);
    EXPECT_FALSE(cache.CreateGPUSceneRasterBindingSnapshot(
        resources, objectConstants, rejected));
    EXPECT_FALSE(rejected.IsReadyForBinding());
    EXPECT_EQ(descriptorCountBeforeFailure, testDevice.createdDescriptorSetDescs.size());
    candidateBuffer->SetReportedStride(candidateStride);
    candidateBuffer->SetReportedSize(candidateSize - candidateStride);
    EXPECT_FALSE(cache.CreateGPUSceneRasterBindingSnapshot(
        resources, objectConstants, rejected));
    EXPECT_FALSE(rejected.IsReadyForBinding());
    EXPECT_EQ(descriptorCountBeforeFailure, testDevice.createdDescriptorSetDescs.size());
    candidateBuffer->SetReportedSize(candidateSize);
    resources.m_exactLeaseVersion = resources.m_leaseVersion - 1u;
    EXPECT_FALSE(cache.CreateGPUSceneRasterBindingSnapshot(
        resources, objectConstants, rejected));
    EXPECT_FALSE(rejected.IsReadyForBinding());
    resources.m_exactLeaseVersion = resources.m_leaseVersion;

    GPUSceneRasterBindingSnapshot binding;
    ASSERT_TRUE(cache.CreateGPUSceneRasterBindingSnapshot(
        resources, objectConstants, binding)) << cache.GetLastError();
    ASSERT_TRUE(binding.IsReadyForBinding());
    EXPECT_EQ(resources.m_leaseVersion, binding.leaseVersion);
    EXPECT_EQ(resources.m_candidateCount, binding.candidateCount);
    EXPECT_EQ(resources.m_primitiveCapacity, binding.primitiveCapacity);
    EXPECT_EQ(resources.m_transformCapacity, binding.transformCapacity);
    EXPECT_NE(nullptr, binding.objectConstantBuffer.Get());
    EXPECT_NE(directObjectSet, binding.objectDescriptorSet.Get());
    EXPECT_NE(directObjectLayout, binding.objectSetLayout.Get());
    EXPECT_EQ(directObjectSet, cache.GetObjectDescriptorSet());
    EXPECT_EQ(directObjectLayout, cache.GetObjectSetLayout());

    const auto findBinding = [](const std::vector<RHIDescriptorBinding>& bindings,
                                uint32 bindingIndex) -> const RHIDescriptorBinding*
    {
        const auto it = std::find_if(
            bindings.begin(), bindings.end(),
            [bindingIndex](const RHIDescriptorBinding& binding)
            {
                return binding.binding == bindingIndex;
            });
        return it == bindings.end() ? nullptr : &(*it);
    };
    const auto& bindings = binding.objectDescriptorSet->GetDescriptorSnapshot();
    const RHIDescriptorBinding* const b0 = findBinding(bindings, 0u);
    const RHIDescriptorBinding* const t1 = findBinding(bindings, 1u);
    const RHIDescriptorBinding* const t2 = findBinding(bindings, 2u);
    const RHIDescriptorBinding* const t3 = findBinding(bindings, 3u);
    ASSERT_NE(nullptr, b0);
    ASSERT_NE(nullptr, t1);
    ASSERT_NE(nullptr, t2);
    ASSERT_NE(nullptr, t3);
    EXPECT_EQ(binding.objectConstantBuffer.Get(), b0->buffer);
    EXPECT_EQ(resources.m_candidates.Get(), t1->buffer);
    EXPECT_EQ(resources.m_primitives.Get(), t2->buffer);
    EXPECT_EQ(resources.m_transforms.Get(), t3->buffer);
    EXPECT_EQ(((sizeof(GPUSceneRasterObjectConstants) + 255ull) & ~255ull),
              b0->range);

    const FakeBuffer* const privateConstants =
        static_cast<const FakeBuffer*>(binding.objectConstantBuffer.Get());
    ASSERT_GE(privateConstants->GetStorage().size(),
              sizeof(GPUSceneRasterObjectConstants));
    GPUSceneRasterObjectConstants uploaded{};
    std::memcpy(&uploaded, privateConstants->GetStorage().data(), sizeof(uploaded));
    EXPECT_EQ(0, std::memcmp(&objectConstants, &uploaded.objectConstants,
                             sizeof(objectConstants)));
    EXPECT_EQ(resources.m_candidateCount, uploaded.gpuSceneRasterCounts[0]);
    EXPECT_EQ(resources.m_primitiveCapacity, uploaded.gpuSceneRasterCounts[1]);
    EXPECT_EQ(resources.m_transformCapacity, uploaded.gpuSceneRasterCounts[2]);
    EXPECT_EQ(0u, uploaded.gpuSceneRasterCounts[3]);

    RenderSubmissionTracker tracker;
    ASSERT_TRUE(tracker.Initialize(&testDevice));
    RenderRetirementQueue retirement;
    ASSERT_TRUE(retirement.Initialize(&tracker));
    RenderSubmissionResourceBatch batch;
    ASSERT_TRUE(binding.RetainSubmissionResources(batch));
    EXPECT_EQ(12u, batch.GetRetainedObjectCount());
    batch.ReleaseUnsubmitted(retirement);
    EXPECT_EQ(0u, batch.GetRetainedObjectCount());

    resources = {};
    cache.Shutdown();
    EXPECT_TRUE(binding.IsReadyForBinding());
    EXPECT_NE(nullptr, binding.objectDescriptorSet.Get());
    EXPECT_NE(nullptr, binding.opaquePipeline.Get());
    EXPECT_NE(nullptr, binding.maskedPipeline.Get());
    EXPECT_NE(nullptr, binding.depthPipeline.Get());
}

TEST_F(RenderPassValidationFixture,
       GPUSceneRasterPipelineFailureClearsPartialStateAndRecoversOnReinitialize)
{
    const fs::path shaderDir = FindShaderDirectory();
    if (shaderDir.empty())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    FakeDevice testDevice;
    PipelineCache cache{RVX::Tests::CreateDeterministicShaderCompiler};
    ASSERT_TRUE(cache.Initialize(&testDevice, shaderDir.string())) << cache.GetLastError();
    ASSERT_TRUE(cache.IsGPUSceneRasterReady());
    ASSERT_TRUE(cache.GetLastError().empty());

    const auto makeStructuredBuffer = [&testDevice](uint32 capacity,
                                                    uint32 stride,
                                                    const char* debugName)
    {
        RHIBufferDesc desc;
        desc.size = static_cast<uint64>(capacity) * stride;
        desc.usage = RHIBufferUsage::Structured | RHIBufferUsage::ShaderResource;
        desc.memoryType = RHIMemoryType::Upload;
        desc.stride = stride;
        desc.debugName = debugName;
        return testDevice.CreateBuffer(desc);
    };

    GPUSceneRasterResourceSnapshot resources;
    resources.m_candidateCount = 1u;
    resources.m_candidateCapacity = 1u;
    resources.m_primitiveCapacity = 1u;
    resources.m_transformCapacity = 1u;
    resources.m_leaseVersion = 97u;
    resources.m_exactLeaseVersion = 97u;
    resources.m_candidates = makeStructuredBuffer(
        resources.m_candidateCapacity, sizeof(GPUSceneCullingCandidate),
        "GPUSceneRasterPipelineFailure.Candidates");
    resources.m_primitives = makeStructuredBuffer(
        resources.m_primitiveCapacity, sizeof(GPUScenePrimitiveRow),
        "GPUSceneRasterPipelineFailure.Primitives");
    resources.m_transforms = makeStructuredBuffer(
        resources.m_transformCapacity, sizeof(GPUSceneTransformRow),
        "GPUSceneRasterPipelineFailure.Transforms");
    ASSERT_TRUE(resources.IsValid());

    // The binding factory creates opaque before masked. Reject only masked so
    // cleanup must remove the successfully cached opaque GPU-scene pipeline.
    testDevice.failGraphicsPipelineDebugName = "GPUSceneMaskedPipeline";
    GPUSceneRasterBindingSnapshot rejected;
    ObjectConstants objectConstants{};
    EXPECT_FALSE(cache.CreateGPUSceneRasterBindingSnapshot(
        resources, objectConstants, rejected));
    EXPECT_FALSE(rejected.IsReadyForBinding());

    const auto opaqueIt = std::find(
        testDevice.createdGraphicsPipelineDebugNames.begin(),
        testDevice.createdGraphicsPipelineDebugNames.end(),
        "GPUSceneOpaquePipeline");
    const auto maskedIt = std::find(
        testDevice.createdGraphicsPipelineDebugNames.begin(),
        testDevice.createdGraphicsPipelineDebugNames.end(),
        "GPUSceneMaskedPipeline");
    ASSERT_NE(opaqueIt, testDevice.createdGraphicsPipelineDebugNames.end());
    ASSERT_NE(maskedIt, testDevice.createdGraphicsPipelineDebugNames.end());
    EXPECT_LT(std::distance(testDevice.createdGraphicsPipelineDebugNames.begin(), opaqueIt),
              std::distance(testDevice.createdGraphicsPipelineDebugNames.begin(), maskedIt));

    EXPECT_TRUE(cache.GetLastError().empty());
    EXPECT_FALSE(cache.IsGPUSceneRasterReady());
    EXPECT_NE(std::string::npos,
              cache.GetGPUSceneRasterUnavailableReason().find("masked pipeline"));
    EXPECT_EQ(nullptr, cache.GetGPUSceneRasterLayout());
    EXPECT_EQ(nullptr, cache.GetGPUSceneRasterObjectSetLayout());
    EXPECT_EQ(nullptr, cache.GetGPUScenePipelineForVariant(
                           MaterialPipelineVariant::Opaque, RHIFormat::RGBA8_UNORM));
    EXPECT_EQ(nullptr, cache.GetGPUSceneDepthOnlyPipeline());
    EXPECT_EQ(nullptr, cache.m_gpuSceneOpaquePipeline.Get());
    EXPECT_EQ(nullptr, cache.m_gpuSceneMaskedPipeline.Get());
    EXPECT_EQ(nullptr, cache.m_gpuSceneDepthOnlyPipeline.Get());
    EXPECT_EQ(nullptr, cache.m_gpuSceneVertexShader.Get());
    EXPECT_EQ(nullptr, cache.m_gpuSceneDepthOnlyVertexShader.Get());
    EXPECT_EQ(nullptr, cache.m_gpuSceneRasterObjectSetLayout.Get());
    EXPECT_EQ(nullptr, cache.m_gpuSceneRasterPipelineLayout.Get());
    EXPECT_TRUE(std::none_of(
        cache.m_pipelineCache.begin(), cache.m_pipelineCache.end(),
        [](const auto& entry)
        {
            const auto* pipeline = static_cast<const FakePipeline*>(entry.second.Get());
            return pipeline && (pipeline->debugName == "GPUSceneOpaquePipeline" ||
                                pipeline->debugName == "GPUSceneMaskedPipeline" ||
                                pipeline->debugName == "GPUSceneDepthOnlyPipeline");
        }));

    // Optional GPU-scene failure must leave Direct and Tier1 submission usable.
    EXPECT_NE(nullptr, cache.GetDefaultLayout());
    EXPECT_NE(nullptr, cache.GetObjectSetLayout());
    EXPECT_NE(nullptr, cache.GetObjectDescriptorSet());
    EXPECT_NE(nullptr, cache.GetPipelineForVariant(
                           MaterialPipelineVariant::Opaque, RHIFormat::RGBA8_UNORM));
    EXPECT_NE(nullptr, cache.GetGPUDrivenPipelineForVariant(
                           MaterialPipelineVariant::Opaque, RHIFormat::RGBA8_UNORM));
    EXPECT_TRUE(cache.GetLastError().empty());

    cache.Shutdown();
    EXPECT_TRUE(cache.GetGPUSceneRasterUnavailableReason().empty());
    testDevice.failGraphicsPipelineDebugName.clear();
    ASSERT_TRUE(cache.Initialize(&testDevice, shaderDir.string())) << cache.GetLastError();
    ASSERT_TRUE(cache.IsGPUSceneRasterReady());
    ASSERT_TRUE(cache.GetLastError().empty());

    GPUSceneRasterBindingSnapshot recovered;
    ASSERT_TRUE(cache.CreateGPUSceneRasterBindingSnapshot(
        resources, objectConstants, recovered));
    EXPECT_TRUE(recovered.IsReadyForBinding());
}

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

    shadowPass.SetEnabled(true);
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

TEST(RenderPassStatusValidation, DefaultGraphAdapterValueCapturesViewsAcrossGraphsAndFrames)
{
    ViewSnapshotPass pass;
    RenderGraph firstGraph;
    RenderGraph secondGraph;
    FakeDevice device;
    firstGraph.SetDevice(&device);
    secondGraph.SetDevice(&device);

    RHITextureDesc targetDesc = RHITextureDesc::RenderTarget(
        4, 4, RHIFormat::RGBA8_UNORM);
    const RGTextureHandle firstTarget = firstGraph.CreateTexture(targetDesc);
    const RGTextureHandle secondTarget = secondGraph.CreateTexture(targetDesc);
    firstGraph.SetExportState(firstTarget, RHIResourceState::RenderTarget);
    secondGraph.SetExportState(secondTarget, RHIResourceState::RenderTarget);

    ViewData firstView;
    firstView.viewportWidth = 640;
    firstView.frameNumber = 41;
    firstView.colorTarget = firstTarget;
    ViewData secondView;
    secondView.viewportWidth = 1280;
    secondView.frameNumber = 42;
    secondView.colorTarget = secondTarget;

    pass.AddToGraph(firstGraph, firstView);
    pass.AddToGraph(secondGraph, secondView);
    // Both caller-owned structs may change after registration.  The graph
    // callbacks retain individual record-time snapshots instead.
    firstView.viewportWidth = 7;
    firstView.frameNumber = 99;
    secondView.viewportWidth = 8;
    secondView.frameNumber = 100;

    firstGraph.Compile();
    secondGraph.Compile();
    RecordingCommandContext firstContext;
    RecordingCommandContext secondContext;
    firstGraph.Execute(firstContext);
    secondGraph.Execute(secondContext);

    ASSERT_EQ(2u, pass.observedViewportWidths.size());
    ASSERT_EQ(2u, pass.observedFrameNumbers.size());
    EXPECT_EQ(640u, pass.observedViewportWidths[0]);
    EXPECT_EQ(41u, pass.observedFrameNumbers[0]);
    EXPECT_EQ(1280u, pass.observedViewportWidths[1]);
    EXPECT_EQ(42u, pass.observedFrameNumbers[1]);
}

TEST(RenderPassStatusValidation,
     DefaultGraphAdapterRejectsForeignRenderGraphResources)
{
    Log::Initialize();
    ViewSnapshotPass pass;
    FakeDevice device;
    RenderGraph sourceGraph;
    RenderGraph targetGraph;
    sourceGraph.SetDevice(&device);
    targetGraph.SetDevice(&device);

    const RGTextureHandle sourceTarget = sourceGraph.CreateTexture(
        RHITextureDesc::RenderTarget(4, 4, RHIFormat::RGBA8_UNORM));
    sourceGraph.Clear();
    ViewData view;
    view.colorTarget = sourceTarget;

    pass.AddToGraph(targetGraph, view);
    targetGraph.Compile();
    EXPECT_TRUE(targetGraph.GetCompileStats().compileValid);

    RecordingCommandContext commands;
    targetGraph.Execute(commands);
    EXPECT_TRUE(pass.observedViewportWidths.empty());
    EXPECT_TRUE(pass.observedFrameNumbers.empty());
    EXPECT_EQ(0u, commands.beginRenderPassCount);
    EXPECT_EQ(0u, commands.drawIndexedCount);
    EXPECT_EQ(0u, commands.drawIndexedIndirectCount);
    Log::Shutdown();
}

TEST(RenderPassStatusValidation, RecordContextRejectsStaleMismatchedAndIncompleteGpuSlices)
{
    RenderGraph graph;
    RenderGraph otherGraph;
    RenderPassRecordContext context;
    context.view.renderGraph = &graph;
    context.identity.graph = &graph;
    context.identity.graphIdentity = graph.GetGraphIdentity();
    context.identity.graphRecordingGeneration = graph.GetRecordingGeneration();
    context.identity.frameSequence = 8;
    context.identity.viewOrdinal = 2;
    context.identity.recordEpoch = 3;

    EXPECT_TRUE(context.IsFrameIdentityValid());

    const RHITextureDesc targetDesc = RHITextureDesc::RenderTarget(
        4, 4, RHIFormat::RGBA8_UNORM);
    const RGTextureHandle currentTarget = graph.CreateTexture(targetDesc);
    const RGTextureHandle foreignTarget = otherGraph.CreateTexture(targetDesc);
    const std::array<RGTextureHandle ViewData::*, 3> viewHandles = {
        &ViewData::colorTarget,
        &ViewData::depthTarget,
        &ViewData::velocityTarget};
    for (RGTextureHandle ViewData::* handle : viewHandles)
    {
        context.view.*handle = currentTarget;
        EXPECT_TRUE(context.IsFrameIdentityValid());
        context.view.*handle = foreignTarget;
        EXPECT_FALSE(context.IsFrameIdentityValid());
        context.view.*handle = {};
    }

    DirectionalShadowRecordOutput directionalShadow;
    directionalShadow.identity = context.identity;
    directionalShadow.shadowMap = foreignTarget;
    EXPECT_FALSE(directionalShadow.IsCompatibleWith(context.identity));
    directionalShadow.shadowMap = currentTarget;
    directionalShadow.enabled = true;
    directionalShadow.shadowMapSize = 4;
    directionalShadow.cascadeViewProjections = {Mat4Identity()};
    directionalShadow.cascadeSplitDepths = {1.0f};
    EXPECT_TRUE(directionalShadow.IsCompatibleWith(context.identity));
    directionalShadow.cascadeSplitDepths.clear();
    EXPECT_FALSE(directionalShadow.IsCompatibleWith(context.identity));

    RayTracedShadowRecordOutput rayTracedShadow;
    rayTracedShadow.identity = context.identity;
    rayTracedShadow.shadowMask = foreignTarget;
    EXPECT_FALSE(rayTracedShadow.IsCompatibleWith(context.identity));

    RenderPassGPUDrivenInputs inputs;
    inputs.identity = context.identity;
    EXPECT_FALSE(inputs.IsCompatibleWith(context.identity));

    inputs.identity.recordEpoch = 2;
    EXPECT_FALSE(inputs.IsCompatibleWith(context.identity));

    context.view.renderGraph = &otherGraph;
    EXPECT_FALSE(context.IsFrameIdentityValid());
}

TEST(RenderPassStatusValidation,
     GraphOwnedFrameSnapshotRebindsPointersAndRejectsClearedGraph)
{
    RenderGraph graph;
    RenderFrameExecutionPlan plan;
    plan.frameSequence = 71;
    plan.viewOrdinal = 3;
    SceneMeshPassPreparation preparation;
    RenderVisibilityResult visibility;
    RenderFrameExecutionReport report;
    report.frameSequence = plan.frameSequence;
    RenderScene scene;
    RenderObject object;
    object.entityId = 99;
    scene.AddObject(object);
    std::vector<RenderDrawItem> opaqueDrawItems(1);
    std::vector<RenderDrawItem> maskedDrawItems(1);

    RenderPassRecordContext context;
    context.view.renderGraph = &graph;
    context.view.renderFrameExecutionPlan = &plan;
    context.view.meshPassPreparation = &preparation;
    context.view.renderVisibility = &visibility;
    context.view.renderFrameExecutionReport = &report;
    context.identity.graph = &graph;
    context.identity.graphIdentity = graph.GetGraphIdentity();
    context.identity.graphRecordingGeneration = graph.GetRecordingGeneration();
    context.identity.frameSequence = plan.frameSequence;
    context.identity.viewOrdinal = plan.viewOrdinal;
    context.identity.recordEpoch = 1;
    context.executionPlan = &plan;
    context.meshPassPreparation = &preparation;
    context.visibility = &visibility;
    context.executionReport = &report;
    context.renderScene = &scene;
    context.opaqueDrawItems = &opaqueDrawItems;
    context.maskedDrawItems = &maskedDrawItems;
    context.results = std::make_shared<RenderPassRecordResults>();
    context.frameSnapshot = MakeRenderPassFrameSnapshot(
        context, *context.results);

    const RenderPassExecutionData execution =
        MakeRenderPassExecutionData(context);
    ASSERT_TRUE(execution.IsFrameIdentityValid());
    ASSERT_NE(nullptr, execution.frameSnapshot);
    EXPECT_EQ(&execution.frameSnapshot->executionPlan,
              execution.view.renderFrameExecutionPlan);
    EXPECT_EQ(&execution.frameSnapshot->meshPassPreparation,
              execution.view.meshPassPreparation);
    EXPECT_EQ(&execution.frameSnapshot->visibility,
              execution.view.renderVisibility);
    EXPECT_EQ(&execution.results->executionReport,
              execution.view.renderFrameExecutionReport);

    // The source A data can change after recording without affecting graph B.
    plan.frameSequence = 999;
    report.frameSequence = 999;
    scene.Clear();
    opaqueDrawItems.clear();
    maskedDrawItems.clear();
    EXPECT_EQ(71u, execution.GetExecutionPlan()->frameSequence);
    EXPECT_EQ(71u, execution.GetExecutionReport()->frameSequence);
    EXPECT_EQ(1u, execution.frameSnapshot->scene.GetObjectCount());
    EXPECT_EQ(1u, execution.frameSnapshot->opaqueDrawItems.size());
    EXPECT_EQ(1u, execution.frameSnapshot->maskedDrawItems.size());

    graph.Clear();
    EXPECT_FALSE(execution.IsFrameIdentityValid());
}

TEST(RenderPassStatusValidation,
     PublishedResultsHoldLatestMatchingRecordAndReleaseReplacedSnapshot)
{
    DepthPrepass depthPass;
    ShadowPass shadowPass;
    OpaquePass opaquePass;
    const RenderPassRecordIdentity recordA{
        nullptr, 11u, 1u, 100u, 0u, 1u};
    const RenderPassRecordIdentity recordB{
        nullptr, 11u, 1u, 101u, 0u, 2u};

    auto first = std::make_shared<RenderPassRecordResults>();
    first->identity = recordA;
    first->depthStats.directDrawCount = 1;
    first->shadowStats.drawCount = 1;
    first->opaqueStats.directDrawCount = 1;
    auto second = std::make_shared<RenderPassRecordResults>();
    second->identity = recordB;
    second->depthStats.directDrawCount = 2;
    // A current disabled recording must replace prior non-zero shadow results.
    second->shadowStats = {};
    second->opaqueStats.directDrawCount = 2;
    const std::weak_ptr<RenderPassRecordResults> firstLifetime = first;

    depthPass.PublishRecordResults(first, recordA);
    shadowPass.PublishRecordResults(first, recordA);
    opaquePass.PublishRecordResults(first, recordA);
    first.reset();
    EXPECT_FALSE(firstLifetime.expired());

    // A mismatched identity cannot overwrite the latest published data.
    depthPass.PublishRecordResults(second, recordA);
    shadowPass.PublishRecordResults(second, recordA);
    opaquePass.PublishRecordResults(second, recordA);
    EXPECT_EQ(1u, depthPass.GetDrawStats().directDrawCount);
    EXPECT_EQ(1u, shadowPass.GetStats().drawCount);
    EXPECT_EQ(1u, opaquePass.GetDrawStats().directDrawCount);

    depthPass.PublishRecordResults(second, recordB);
    shadowPass.PublishRecordResults(second, recordB);
    opaquePass.PublishRecordResults(second, recordB);
    EXPECT_EQ(2u, depthPass.GetDrawStats().directDrawCount);
    EXPECT_EQ(0u, shadowPass.GetStats().drawCount);
    EXPECT_EQ(2u, opaquePass.GetDrawStats().directDrawCount);
    EXPECT_TRUE(firstLifetime.expired());
}

TEST_F(RenderPassValidationFixture, MigratedDepthAndOpaquePassesFailClosedForMissingGpuDeclarations)
{
    const std::vector<RenderDrawItem> emptyDrawItems;
    const auto makeContext = [this, &emptyDrawItems](RenderGraph& graph,
                                    RenderFrameExecutionPlan& plan,
                                    SceneMeshPassPreparation& preparation,
                                    RenderVisibilityResult& visibility,
                                    RenderFrameExecutionReport& report,
                                    RenderPassKind passKind,
                                    uint32 directPacketCount = 0)
    {
        plan.frameSequence = 77;
        plan.viewOrdinal = 0;
        RenderPassExecutionPlan passPlan;
        passPlan.pass = passKind;
        passPlan.partition.gpuDrivenPacketCount = 1;
        passPlan.partition.directPacketCount = directPacketCount;
        plan.passes = {passPlan};
        report.frameSequence = plan.frameSequence;
        report.passes = {{passKind}};

        RenderPassRecordContext context;
        context.view.renderGraph = &graph;
        context.view.renderFrameExecutionPlan = &plan;
        context.view.meshPassPreparation = &preparation;
        context.view.renderVisibility = &visibility;
        context.view.renderFrameExecutionReport = &report;
        context.identity.graph = &graph;
        context.identity.graphIdentity = graph.GetGraphIdentity();
        context.identity.graphRecordingGeneration = graph.GetRecordingGeneration();
        context.identity.frameSequence = plan.frameSequence;
        context.identity.viewOrdinal = plan.viewOrdinal;
        context.identity.recordEpoch = 9;
        context.executionPlan = &plan;
        context.meshPassPreparation = &preparation;
        context.visibility = &visibility;
        context.executionReport = &report;
        context.renderScene = &scene;
        context.opaqueDrawItems = &emptyDrawItems;
        context.maskedDrawItems = &emptyDrawItems;
        context.directionalShadow.identity = context.identity;
        context.rayTracedShadow.identity = context.identity;
        if (passKind == RenderPassKind::Depth)
        {
            context.view.depthTarget = graph.CreateTexture(
                RHITextureDesc::DepthStencil(
                    4, 4, PipelineCache::GetDefaultDepthStencilFormat()));
        }
        else if (passKind == RenderPassKind::Opaque)
        {
            context.view.colorTarget = graph.CreateTexture(
                RHITextureDesc::RenderTarget(4, 4, RHIFormat::RGBA8_UNORM));
        }
        context.results = std::make_shared<RenderPassRecordResults>();
        context.frameSnapshot = MakeRenderPassFrameSnapshot(
            context, *context.results);
        return context;
    };
    RenderGraph depthGraph;
    depthGraph.SetDevice(&device);
    RenderFrameExecutionPlan depthPlan;
    SceneMeshPassPreparation depthPreparation;
    RenderVisibilityResult depthVisibility;
    RenderFrameExecutionReport depthReport;
    RenderPassRecordContext depthContext = makeContext(
        depthGraph, depthPlan, depthPreparation, depthVisibility, depthReport,
        RenderPassKind::Depth);
    depthContext.depthGPUDriven.identity = depthContext.identity;
    // No owner or handles: a planned GPU lane must not record an undeclared
    // indirect consumer.
    DepthPrepass depthPass;
    depthPass.AddToGraph(depthGraph, depthContext);
    EXPECT_EQ(RenderPolicyReason::InconsistentFacts,
              depthContext.results->depthStats.failureReason);
    EXPECT_EQ(RenderExecutionStatus::Failed,
              depthContext.results->executionReport.status);
    const RenderPassExecutionReport* depthExecution =
        FindPassExecutionReport(depthContext.results->executionReport,
                                RenderPassKind::Depth);
    ASSERT_NE(depthExecution, nullptr);
    EXPECT_EQ(RenderExecutionStatus::Failed,
              depthExecution->gpuDrivenLane.status);
    EXPECT_EQ(RenderPolicyReason::InconsistentFacts,
              depthExecution->gpuDrivenLane.reason);
    EXPECT_TRUE(depthExecution->gpuDrivenLane.executedCountsAvailable);
    EXPECT_EQ(0u, depthExecution->gpuDrivenLane.executedPacketCount);
    EXPECT_EQ(0u, depthExecution->gpuDrivenLane.executedDrawCount);
    EXPECT_EQ(RenderExecutionStatus::NotAttempted,
              depthExecution->directLane.status);
    EXPECT_EQ(RenderPolicyReason::None, depthExecution->directLane.reason);
    EXPECT_FALSE(depthExecution->directLane.executedCountsAvailable);
    EXPECT_EQ(0u, depthExecution->directLane.executedPacketCount);
    EXPECT_EQ(0u, depthExecution->directLane.executedDrawCount);
    depthGraph.Compile();
    ASSERT_TRUE(depthGraph.GetCompileStats().compileValid);
    RecordingCommandContext depthCommands;
    depthGraph.Execute(depthCommands);
    EXPECT_EQ(0u, depthCommands.beginRenderPassCount);
    EXPECT_EQ(0u, depthCommands.drawIndexedCount);
    EXPECT_EQ(0u, depthCommands.drawIndexedIndirectCount);

    RenderGraph opaqueGraph;
    opaqueGraph.SetDevice(&device);
    RenderFrameExecutionPlan opaquePlan;
    SceneMeshPassPreparation opaquePreparation;
    RenderVisibilityResult opaqueVisibility;
    RenderFrameExecutionReport opaqueReport;
    RenderPassRecordContext opaqueContext = makeContext(
        opaqueGraph, opaquePlan, opaquePreparation, opaqueVisibility, opaqueReport,
        RenderPassKind::Opaque);
    opaqueContext.opaqueGPUDriven.identity = opaqueContext.identity;
    OpaquePass opaquePass;
    opaquePass.AddToGraph(opaqueGraph, opaqueContext);
    EXPECT_EQ(RenderPolicyReason::InconsistentFacts,
              opaqueContext.results->opaqueStats.failureReason);
    EXPECT_EQ(RenderExecutionStatus::Failed,
              opaqueContext.results->executionReport.status);
    const RenderPassExecutionReport* opaqueExecution =
        FindPassExecutionReport(opaqueContext.results->executionReport,
                                RenderPassKind::Opaque);
    ASSERT_NE(opaqueExecution, nullptr);
    EXPECT_EQ(RenderExecutionStatus::Failed,
              opaqueExecution->gpuDrivenLane.status);
    EXPECT_EQ(RenderPolicyReason::InconsistentFacts,
              opaqueExecution->gpuDrivenLane.reason);
    EXPECT_TRUE(opaqueExecution->gpuDrivenLane.executedCountsAvailable);
    EXPECT_EQ(0u, opaqueExecution->gpuDrivenLane.executedPacketCount);
    EXPECT_EQ(0u, opaqueExecution->gpuDrivenLane.executedDrawCount);
    EXPECT_EQ(RenderExecutionStatus::NotAttempted,
              opaqueExecution->directLane.status);
    EXPECT_EQ(RenderPolicyReason::None, opaqueExecution->directLane.reason);
    EXPECT_FALSE(opaqueExecution->directLane.executedCountsAvailable);
    EXPECT_EQ(0u, opaqueExecution->directLane.executedPacketCount);
    EXPECT_EQ(0u, opaqueExecution->directLane.executedDrawCount);
    opaqueGraph.Compile();
    ASSERT_TRUE(opaqueGraph.GetCompileStats().compileValid);
    RecordingCommandContext opaqueCommands;
    opaqueGraph.Execute(opaqueCommands);
    EXPECT_EQ(0u, opaqueCommands.beginRenderPassCount);
    EXPECT_EQ(0u, opaqueCommands.drawIndexedCount);
    EXPECT_EQ(0u, opaqueCommands.drawIndexedIndirectCount);

    RenderGraph hybridGraph;
    hybridGraph.SetDevice(&device);
    RenderFrameExecutionPlan hybridPlan;
    SceneMeshPassPreparation hybridPreparation;
    RenderVisibilityResult hybridVisibility;
    RenderFrameExecutionReport hybridReport;
    RenderPassRecordContext hybridContext = makeContext(
        hybridGraph, hybridPlan, hybridPreparation, hybridVisibility, hybridReport,
        RenderPassKind::Depth, 1);
    hybridContext.depthGPUDriven.identity = hybridContext.identity;
    DepthPrepass hybridPass;
    hybridPass.AddToGraph(hybridGraph, hybridContext);
    const RenderPassExecutionReport* hybridExecution =
        FindPassExecutionReport(hybridContext.results->executionReport,
                                RenderPassKind::Depth);
    ASSERT_NE(hybridExecution, nullptr);
    EXPECT_EQ(RenderExecutionStatus::Failed,
              hybridExecution->gpuDrivenLane.status);
    EXPECT_EQ(RenderPolicyReason::InconsistentFacts,
              hybridExecution->gpuDrivenLane.reason);
    EXPECT_TRUE(hybridExecution->gpuDrivenLane.executedCountsAvailable);
    EXPECT_EQ(0u, hybridExecution->gpuDrivenLane.executedPacketCount);
    EXPECT_EQ(0u, hybridExecution->gpuDrivenLane.executedDrawCount);
    EXPECT_EQ(RenderExecutionStatus::Failed,
              hybridExecution->directLane.status);
    EXPECT_EQ(RenderPolicyReason::InconsistentFacts,
              hybridExecution->directLane.reason);
    EXPECT_TRUE(hybridExecution->directLane.executedCountsAvailable);
    EXPECT_EQ(0u, hybridExecution->directLane.executedPacketCount);
    EXPECT_EQ(0u, hybridExecution->directLane.executedDrawCount);

    RenderGraph unmatchedReportGraph;
    unmatchedReportGraph.SetDevice(&device);
    RenderFrameExecutionPlan unmatchedReportPlan;
    SceneMeshPassPreparation unmatchedReportPreparation;
    RenderVisibilityResult unmatchedReportVisibility;
    RenderFrameExecutionReport unmatchedReport;
    RenderPassRecordContext unmatchedReportContext = makeContext(
        unmatchedReportGraph,
        unmatchedReportPlan,
        unmatchedReportPreparation,
        unmatchedReportVisibility,
        unmatchedReport,
        RenderPassKind::Depth);
    unmatchedReport.frameSequence = 0;
    unmatchedReport.passes = {{RenderPassKind::Opaque}};
    unmatchedReportContext.depthGPUDriven.identity = unmatchedReportContext.identity;
    DepthPrepass unmatchedReportPass;
    unmatchedReportPass.AddToGraph(unmatchedReportGraph, unmatchedReportContext);
    EXPECT_EQ(RenderExecutionStatus::Failed,
              unmatchedReportContext.results->executionReport.status);
    EXPECT_EQ(unmatchedReportContext.identity.frameSequence,
              unmatchedReportContext.results->executionReport.frameSequence);
}

TEST_F(RenderPassValidationFixture,
       MigratedDepthAndOpaquePassesRejectBaseViewDataAdapterThroughIRenderPass)
{
    const auto expectLegacyAdapterRejection = [this](
                                                IRenderPass* pass,
                                                RenderGraph& graph,
                                                const ViewData& legacyView)
    {
        ASSERT_NE(pass, nullptr);
        pass->AddToGraph(graph, legacyView);
        graph.Compile();
        ASSERT_TRUE(graph.GetCompileStats().compileValid);
        const RenderGraph::Diagnostics diagnostics = graph.GetDiagnostics();
        ASSERT_EQ(1u, diagnostics.passes.size());
        EXPECT_TRUE(diagnostics.passes[0].usages.empty());

        RecordingCommandContext commands;
        graph.Execute(commands);
        EXPECT_EQ(0u, commands.beginRenderPassCount);
        EXPECT_EQ(0u, commands.drawCount);
        EXPECT_EQ(0u, commands.drawIndexedCount);
        EXPECT_EQ(0u, commands.drawIndexedIndirectCount);
    };
    const auto makeLegacyView = [](RenderGraph& graph,
                                   RenderPassKind passKind,
                                   RenderFrameExecutionPlan& plan,
                                   SceneMeshPassPreparation& preparation,
                                   RenderVisibilityResult& visibility,
                                   RenderFrameExecutionReport& report)
    {
        plan.frameSequence = 88;
        plan.viewOrdinal = 0;
        plan.passes = {{passKind}};
        report.frameSequence = plan.frameSequence;
        report.passes = {{passKind}};

        ViewData legacyView;
        legacyView.renderGraph = &graph;
        legacyView.renderFrameExecutionPlan = &plan;
        legacyView.meshPassPreparation = &preparation;
        legacyView.renderVisibility = &visibility;
        legacyView.renderFrameExecutionReport = &report;
        legacyView.depthTarget = graph.CreateTexture(
            RHITextureDesc::DepthStencil(
                4, 4, PipelineCache::GetDefaultDepthStencilFormat()));
        if (passKind == RenderPassKind::Opaque)
        {
            legacyView.colorTarget = graph.CreateTexture(
                RHITextureDesc::RenderTarget(4, 4, RHIFormat::RGBA8_UNORM));
        }
        return legacyView;
    };

    RenderGraph depthGraph;
    depthGraph.SetDevice(&device);
    RenderFrameExecutionPlan depthPlan;
    SceneMeshPassPreparation depthPreparation;
    RenderVisibilityResult depthVisibility;
    RenderFrameExecutionReport depthReport;
    const ViewData depthView = makeLegacyView(
        depthGraph, RenderPassKind::Depth, depthPlan, depthPreparation,
        depthVisibility, depthReport);
    DepthPrepass depthPass;
    IRenderPass* const depthBasePass = &depthPass;
    expectLegacyAdapterRejection(depthBasePass, depthGraph, depthView);

    RenderGraph opaqueGraph;
    opaqueGraph.SetDevice(&device);
    RenderFrameExecutionPlan opaquePlan;
    SceneMeshPassPreparation opaquePreparation;
    RenderVisibilityResult opaqueVisibility;
    RenderFrameExecutionReport opaqueReport;
    const ViewData opaqueView = makeLegacyView(
        opaqueGraph, RenderPassKind::Opaque, opaquePlan, opaquePreparation,
        opaqueVisibility, opaqueReport);
    OpaquePass opaquePass;
    IRenderPass* const opaqueBasePass = &opaquePass;
    expectLegacyAdapterRejection(opaqueBasePass, opaqueGraph, opaqueView);
}

TEST_F(RenderPassValidationFixture,
       MigratedPassesRejectForeignAndStaleRecordGraphs)
{
    const std::vector<RenderDrawItem> emptyDrawItems;
    const auto makeContext = [this, &emptyDrawItems](
                                 RenderGraph& graph,
                                 RenderFrameExecutionPlan& plan,
                                 SceneMeshPassPreparation& preparation,
                                 RenderVisibilityResult& visibility,
                                 RenderFrameExecutionReport& report,
                                 RenderPassKind passKind)
    {
        plan.frameSequence = 451;
        plan.viewOrdinal = 3;
        plan.passes = {{passKind}};
        report.frameSequence = plan.frameSequence;
        report.passes = {{passKind}};

        RenderPassRecordContext context;
        context.view.renderGraph = &graph;
        context.view.renderFrameExecutionPlan = &plan;
        context.view.meshPassPreparation = &preparation;
        context.view.renderVisibility = &visibility;
        context.view.renderFrameExecutionReport = &report;
        context.identity.graph = &graph;
        context.identity.graphIdentity = graph.GetGraphIdentity();
        context.identity.graphRecordingGeneration =
            graph.GetRecordingGeneration();
        context.identity.frameSequence = plan.frameSequence;
        context.identity.viewOrdinal = plan.viewOrdinal;
        context.identity.recordEpoch = 19;
        context.executionPlan = &plan;
        context.meshPassPreparation = &preparation;
        context.visibility = &visibility;
        context.executionReport = &report;
        context.renderScene = &scene;
        context.opaqueDrawItems = &emptyDrawItems;
        context.maskedDrawItems = &emptyDrawItems;
        context.directionalShadow.identity = context.identity;
        context.rayTracedShadow.identity = context.identity;
        context.results = std::make_shared<RenderPassRecordResults>();
        return context;
    };

    RenderGraph sourceGraph;
    RenderGraph targetGraph;
    sourceGraph.SetDevice(&device);
    targetGraph.SetDevice(&device);
    RenderFrameExecutionPlan depthPlan;
    SceneMeshPassPreparation depthPreparation;
    RenderVisibilityResult depthVisibility;
    RenderFrameExecutionReport depthReport;
    RenderPassRecordContext foreignDepthContext = makeContext(
        sourceGraph,
        depthPlan,
        depthPreparation,
        depthVisibility,
        depthReport,
        RenderPassKind::Depth);

    DepthPrepass depthPass;
    const DepthPrepassDrawStats foreignDepthStatsBefore =
        foreignDepthContext.results->depthStats;
    const RenderFrameExecutionReport foreignDepthReportBefore =
        foreignDepthContext.results->executionReport;
    depthPass.AddToGraph(targetGraph, foreignDepthContext);
    EXPECT_EQ(foreignDepthStatsBefore.failureReason,
              foreignDepthContext.results->depthStats.failureReason);
    EXPECT_EQ(foreignDepthReportBefore.status,
              foreignDepthContext.results->executionReport.status);
    targetGraph.Compile();
    ASSERT_TRUE(targetGraph.GetCompileStats().compileValid);
    RecordingCommandContext depthCommands;
    targetGraph.Execute(depthCommands);
    EXPECT_EQ(0u, depthCommands.beginRenderPassCount);
    EXPECT_EQ(0u, depthCommands.drawIndexedCount);
    EXPECT_EQ(0u, depthCommands.drawIndexedIndirectCount);

    // A malformed pair with a non-null snapshot but missing results must be
    // rejected before any snapshot/report dereference or graph declaration.
    RenderGraph missingResultsGraph;
    missingResultsGraph.SetDevice(&device);
    RenderFrameExecutionPlan missingResultsPlan;
    SceneMeshPassPreparation missingResultsPreparation;
    RenderVisibilityResult missingResultsVisibility;
    RenderFrameExecutionReport missingResultsReport;
    RenderPassRecordContext missingResultsContext = makeContext(
        missingResultsGraph,
        missingResultsPlan,
        missingResultsPreparation,
        missingResultsVisibility,
        missingResultsReport,
        RenderPassKind::Opaque);
    const std::shared_ptr<RenderPassRecordResults> missingResultsOwner =
        missingResultsContext.results;
    missingResultsContext.frameSnapshot = MakeRenderPassFrameSnapshot(
        missingResultsContext, *missingResultsOwner);
    missingResultsContext.results.reset();
    OpaquePass missingResultsPass;
    missingResultsPass.AddToGraph(missingResultsGraph, missingResultsContext);
    missingResultsGraph.Compile();
    ASSERT_TRUE(missingResultsGraph.GetCompileStats().compileValid);
    ASSERT_EQ(1u, missingResultsGraph.GetDiagnostics().passes.size());
    EXPECT_TRUE(missingResultsGraph.GetDiagnostics().passes[0].usages.empty());
    RecordingCommandContext missingResultsCommands;
    missingResultsGraph.Execute(missingResultsCommands);
    EXPECT_EQ(0u, missingResultsCommands.beginRenderPassCount);

    // A fully paired Opaque source remains owned by its source graph when it
    // is registered into a different target graph. The generic helper must
    // not rewrite its results while rejecting the foreign registration.
    RenderGraph opaqueSourceGraph;
    RenderGraph opaqueTargetGraph;
    opaqueSourceGraph.SetDevice(&device);
    opaqueTargetGraph.SetDevice(&device);
    RenderFrameExecutionPlan foreignOpaquePlan;
    SceneMeshPassPreparation foreignOpaquePreparation;
    RenderVisibilityResult foreignOpaqueVisibility;
    RenderFrameExecutionReport foreignOpaqueReport;
    RenderPassRecordContext foreignOpaqueContext = makeContext(
        opaqueSourceGraph,
        foreignOpaquePlan,
        foreignOpaquePreparation,
        foreignOpaqueVisibility,
        foreignOpaqueReport,
        RenderPassKind::Opaque);
    foreignOpaqueContext.view.colorTarget = opaqueSourceGraph.CreateTexture(
        RHITextureDesc::RenderTarget(4, 4, RHIFormat::RGBA8_UNORM));
    opaqueSourceGraph.SetExportState(
        foreignOpaqueContext.view.colorTarget, RHIResourceState::RenderTarget);
    foreignOpaqueContext.frameSnapshot = MakeRenderPassFrameSnapshot(
        foreignOpaqueContext, *foreignOpaqueContext.results);
    ASSERT_TRUE(foreignOpaqueContext.MatchesTargetGraph(opaqueSourceGraph));
    ASSERT_TRUE(foreignOpaqueContext.IsFrameIdentityValid());
    const RenderPassRecordIdentity foreignOpaqueIdentity =
        foreignOpaqueContext.results->identity;
    foreignOpaqueContext.results->opaqueStats.directDrawCount = 74;
    foreignOpaqueContext.results->opaqueStats.failureReason = RenderPolicyReason::None;
    foreignOpaqueContext.results->opaqueShadowStats.requested = true;
    foreignOpaqueContext.results->directionalShadowOutput.enabled = true;
    foreignOpaqueContext.results->directionalShadowOutput.shadowMapSize = 76;
    foreignOpaqueContext.results->executionReport.status = RenderExecutionStatus::Completed;
    foreignOpaqueContext.results->executionReport.frameSequence =
        foreignOpaqueContext.identity.frameSequence;
    OpaquePass foreignOpaquePass;
    foreignOpaquePass.AddToGraph(opaqueTargetGraph, foreignOpaqueContext);
    opaqueTargetGraph.Compile();
    ASSERT_TRUE(opaqueTargetGraph.GetCompileStats().compileValid);
    ASSERT_EQ(1u, opaqueTargetGraph.GetDiagnostics().passes.size());
    EXPECT_TRUE(opaqueTargetGraph.GetDiagnostics().passes[0].usages.empty());
    RecordingCommandContext foreignOpaqueCommands;
    opaqueTargetGraph.Execute(foreignOpaqueCommands);
    EXPECT_EQ(0u, foreignOpaqueCommands.beginRenderPassCount);
    EXPECT_EQ(foreignOpaqueIdentity, foreignOpaqueContext.results->identity);
    EXPECT_EQ(74u, foreignOpaqueContext.results->opaqueStats.directDrawCount);
    EXPECT_EQ(RenderPolicyReason::None,
              foreignOpaqueContext.results->opaqueStats.failureReason);
    EXPECT_TRUE(foreignOpaqueContext.results->opaqueShadowStats.requested);
    EXPECT_TRUE(foreignOpaqueContext.results->directionalShadowOutput.enabled);
    EXPECT_EQ(76u,
              foreignOpaqueContext.results->directionalShadowOutput.shadowMapSize);
    EXPECT_EQ(RenderExecutionStatus::Completed,
              foreignOpaqueContext.results->executionReport.status);
    EXPECT_EQ(foreignOpaqueContext.identity.frameSequence,
              foreignOpaqueContext.results->executionReport.frameSequence);

    RenderGraph staleGraph;
    staleGraph.SetDevice(&device);
    RenderFrameExecutionPlan opaquePlan;
    SceneMeshPassPreparation opaquePreparation;
    RenderVisibilityResult opaqueVisibility;
    RenderFrameExecutionReport opaqueReport;
    RenderPassRecordContext staleOpaqueContext = makeContext(
        staleGraph,
        opaquePlan,
        opaquePreparation,
        opaqueVisibility,
        opaqueReport,
        RenderPassKind::Opaque);
    staleOpaqueContext.view.colorTarget = staleGraph.CreateTexture(
        RHITextureDesc::RenderTarget(4, 4, RHIFormat::RGBA8_UNORM));
    staleOpaqueContext.frameSnapshot = MakeRenderPassFrameSnapshot(
        staleOpaqueContext, *staleOpaqueContext.results);
    ASSERT_TRUE(staleOpaqueContext.MatchesTargetGraph(staleGraph));
    ASSERT_TRUE(staleOpaqueContext.IsFrameIdentityValid());
    // A stale typed source is owned by its original recording. Rejecting it
    // must not let OpaquePass synthesize a snapshot or overwrite diagnostics.
    staleOpaqueContext.results->opaqueStats.directDrawCount = 73;
    staleOpaqueContext.results->opaqueStats.failureReason = RenderPolicyReason::None;
    staleOpaqueContext.results->opaqueShadowStats.requested = true;
    staleOpaqueContext.results->executionReport.status = RenderExecutionStatus::Completed;
    staleOpaqueContext.results->executionReport.frameSequence =
        staleOpaqueContext.identity.frameSequence;
    const RenderPassRecordIdentity staleResultsIdentity =
        staleOpaqueContext.results->identity;
    staleGraph.Clear();

    OpaquePass opaquePass;
    opaquePass.AddToGraph(staleGraph, staleOpaqueContext);
    EXPECT_EQ(staleResultsIdentity, staleOpaqueContext.results->identity);
    EXPECT_EQ(73u, staleOpaqueContext.results->opaqueStats.directDrawCount);
    EXPECT_EQ(RenderPolicyReason::None,
              staleOpaqueContext.results->opaqueStats.failureReason);
    EXPECT_TRUE(staleOpaqueContext.results->opaqueShadowStats.requested);
    EXPECT_EQ(RenderExecutionStatus::Completed,
              staleOpaqueContext.results->executionReport.status);
    EXPECT_EQ(staleOpaqueContext.identity.frameSequence,
              staleOpaqueContext.results->executionReport.frameSequence);
    staleGraph.Compile();
    ASSERT_TRUE(staleGraph.GetCompileStats().compileValid);
    RecordingCommandContext opaqueCommands;
    staleGraph.Execute(opaqueCommands);
    EXPECT_EQ(0u, opaqueCommands.beginRenderPassCount);
    EXPECT_EQ(0u, opaqueCommands.drawIndexedCount);
    EXPECT_EQ(0u, opaqueCommands.drawIndexedIndirectCount);
}

TEST_F(RenderPassValidationFixture,
       OpaquePassRejectsInvalidForeignAndStaleShadowInputs)
{
    const std::vector<RenderDrawItem> emptyDrawItems;
    const RHITextureDesc shadowDesc = RHITextureDesc::Texture2D(
        4, 4, RHIFormat::R8_UNORM);
    const auto makeContext = [this, &emptyDrawItems](
                                 RenderGraph& graph,
                                 RenderFrameExecutionPlan& plan,
                                 SceneMeshPassPreparation& preparation,
                                 RenderVisibilityResult& visibility,
                                 RenderFrameExecutionReport& report)
    {
        plan.frameSequence = 452;
        plan.viewOrdinal = 4;
        plan.passes = {{RenderPassKind::Opaque}};
        report.frameSequence = plan.frameSequence;
        report.passes = {{RenderPassKind::Opaque}};

        RenderPassRecordContext context;
        context.view.renderGraph = &graph;
        context.view.renderFrameExecutionPlan = &plan;
        context.view.meshPassPreparation = &preparation;
        context.view.renderVisibility = &visibility;
        context.view.renderFrameExecutionReport = &report;
        context.identity.graph = &graph;
        context.identity.graphIdentity = graph.GetGraphIdentity();
        context.identity.graphRecordingGeneration =
            graph.GetRecordingGeneration();
        context.identity.frameSequence = plan.frameSequence;
        context.identity.viewOrdinal = plan.viewOrdinal;
        context.identity.recordEpoch = 20;
        context.executionPlan = &plan;
        context.meshPassPreparation = &preparation;
        context.visibility = &visibility;
        context.executionReport = &report;
        context.renderScene = &scene;
        context.opaqueDrawItems = &emptyDrawItems;
        context.maskedDrawItems = &emptyDrawItems;
        context.directionalShadow.identity = context.identity;
        context.rayTracedShadow.identity = context.identity;
        context.view.colorTarget = graph.CreateTexture(
            RHITextureDesc::RenderTarget(4, 4, RHIFormat::RGBA8_UNORM));
        context.results = std::make_shared<RenderPassRecordResults>();
        context.frameSnapshot = MakeRenderPassFrameSnapshot(
            context, *context.results);
        return context;
    };
    const auto renewCurrentGraphSource = [](RenderGraph& graph,
                                            RenderPassRecordContext& context)
    {
        context.view.colorTarget = graph.CreateTexture(
            RHITextureDesc::RenderTarget(4, 4, RHIFormat::RGBA8_UNORM));
        context.identity.graph = &graph;
        context.identity.graphIdentity = graph.GetGraphIdentity();
        context.identity.graphRecordingGeneration = graph.GetRecordingGeneration();
        context.directionalShadow.identity = context.identity;
        context.rayTracedShadow.identity = context.identity;
        context.frameSnapshot = MakeRenderPassFrameSnapshot(
            context, *context.results);
    };
    const auto expectRejected = [this, &makeContext](
                                    const char* caseName,
                                    const auto& configureContext)
    {
        SCOPED_TRACE(caseName);
        RenderGraph graph;
        graph.SetDevice(&device);
        RenderFrameExecutionPlan plan;
        SceneMeshPassPreparation preparation;
        RenderVisibilityResult visibility;
        RenderFrameExecutionReport report;
        RenderPassRecordContext context = makeContext(
            graph, plan, preparation, visibility, report);
        configureContext(graph, context);

        OpaquePass pass;
        pass.AddToGraph(graph, context);
        EXPECT_EQ(RenderPolicyReason::InconsistentFacts,
                  context.results->opaqueStats.failureReason);
        EXPECT_EQ(RenderExecutionStatus::Failed,
                  context.results->executionReport.status);
        graph.Compile();
        ASSERT_TRUE(graph.GetCompileStats().compileValid);
        RecordingCommandContext commands;
        graph.Execute(commands);
        EXPECT_EQ(0u, commands.beginRenderPassCount);
        EXPECT_EQ(0u, commands.drawIndexedCount);
        EXPECT_EQ(0u, commands.drawIndexedIndirectCount);
    };

    expectRejected("directional invalid",
                   [](RenderGraph&, RenderPassRecordContext& context)
                   {
                       context.directionalShadow.enabled = true;
                       context.directionalShadow.shadowMapSize = 4;
                       context.directionalShadow.cascadeViewProjections = {
                           Mat4Identity()};
                       context.directionalShadow.cascadeSplitDepths = {1.0f};
                   });

    RenderGraph foreignGraph;
    const RGTextureHandle foreignShadow = foreignGraph.CreateTexture(shadowDesc);
    expectRejected("directional foreign",
                   [foreignShadow](RenderGraph&, RenderPassRecordContext& context)
                   {
                       context.directionalShadow.enabled = true;
                       context.directionalShadow.shadowMap = foreignShadow;
                       context.directionalShadow.shadowMapSize = 4;
                       context.directionalShadow.cascadeViewProjections = {
                           Mat4Identity()};
                       context.directionalShadow.cascadeSplitDepths = {1.0f};
                   });

    expectRejected("directional stale",
                   [shadowDesc, &renewCurrentGraphSource](RenderGraph& graph,
                                                           RenderPassRecordContext& context)
                   {
                       const RGTextureHandle staleShadow =
                           graph.CreateTexture(shadowDesc);
                       graph.Clear();
                       renewCurrentGraphSource(graph, context);
                       context.directionalShadow.enabled = true;
                       context.directionalShadow.shadowMap = staleShadow;
                       context.directionalShadow.shadowMapSize = 4;
                       context.directionalShadow.cascadeViewProjections = {
                           Mat4Identity()};
                       context.directionalShadow.cascadeSplitDepths = {1.0f};
                   });

    expectRejected("ray traced invalid",
                   [](RenderGraph&, RenderPassRecordContext& context)
                   {
                       context.rayTracedShadow.enabled = true;
                   });

    expectRejected("ray traced foreign",
                   [foreignShadow](RenderGraph&, RenderPassRecordContext& context)
                   {
                       context.rayTracedShadow.enabled = true;
                       context.rayTracedShadow.shadowMask = foreignShadow;
                   });

    expectRejected("ray traced stale",
                   [shadowDesc, &renewCurrentGraphSource](RenderGraph& graph,
                                                           RenderPassRecordContext& context)
                   {
                       const RGTextureHandle staleShadow =
                           graph.CreateTexture(shadowDesc);
                       graph.Clear();
                       renewCurrentGraphSource(graph, context);
                       context.rayTracedShadow.enabled = true;
                       context.rayTracedShadow.shadowMask = staleShadow;
                   });
}

TEST_F(RenderPassValidationFixture, MigratedDepthPassExecutesItsOwnedRecordContext)
{
    RVX_REQUIRE_RENDER_RUNTIME_PIPELINE();

    RenderGraph graph;
    graph.SetDevice(&device);
    RHITextureDesc depthDesc = RHITextureDesc::DepthStencil(
        32, 32, PipelineCache::GetDefaultDepthStencilFormat());
    const RGTextureHandle depthTarget = graph.CreateTexture(depthDesc);
    graph.SetExportState(depthTarget, RHIResourceState::DepthRead);

    const std::vector<RenderDrawItem> emptyDrawItems;
    const SceneMeshPassPreparation preparation = PrepareDepthPackets(
        emptyDrawItems, emptyDrawItems);
    const RenderFramePlanCompileResult compiled = CompileForcedDirectPlan(
        preparation, 78);
    ASSERT_TRUE(compiled.succeeded);
    RenderVisibilityResult visibility;
    RenderFrameExecutionReport report = MakeExecutionReport(compiled.plan);

    RenderPassRecordContext context;
    context.view.renderGraph = &graph;
    context.view.viewCache = &viewCache;
    context.view.depthTarget = depthTarget;
    context.view.viewportWidth = 32;
    context.view.viewportHeight = 32;
    context.view.renderFrameExecutionPlan = &compiled.plan;
    context.view.meshPassPreparation = &preparation;
    context.view.renderVisibility = &visibility;
    context.view.renderFrameExecutionReport = &report;
    context.identity.graph = &graph;
    context.identity.graphIdentity = graph.GetGraphIdentity();
    context.identity.graphRecordingGeneration = graph.GetRecordingGeneration();
    context.identity.frameSequence = compiled.plan.frameSequence;
    context.identity.recordEpoch = 10;
    context.executionPlan = &compiled.plan;
    context.meshPassPreparation = &preparation;
    context.visibility = &visibility;
    context.executionReport = &report;
    context.renderScene = &scene;
    context.opaqueDrawItems = &emptyDrawItems;
    context.maskedDrawItems = &emptyDrawItems;
    context.results = std::make_shared<RenderPassRecordResults>();
    context.directionalShadow.identity = context.identity;
    context.rayTracedShadow.identity = context.identity;
    context.frameSnapshot = MakeRenderPassFrameSnapshot(
        context, *context.results);

    DepthPrepass pass;
    pass.SetEnabled(true);
    ConfigureResources(pass, gpuResources, pipelineCache);
    pass.SetMaterialSystem(&materialSystem);
    pass.AddToGraph(graph, context);
    graph.Compile();
    ASSERT_TRUE(graph.GetCompileStats().compileValid);

    RecordingCommandContext commandContext;
    graph.Execute(commandContext);
    EXPECT_EQ(1u, commandContext.beginRenderPassCount);
    EXPECT_TRUE(context.results->depthStats.planValidated);
    EXPECT_EQ(RenderPolicyReason::None, context.results->depthStats.failureReason);
    const RenderPassExecutionReport* depthReport =
        FindPassExecutionReport(context.results->executionReport,
                                RenderPassKind::Depth);
    ASSERT_NE(depthReport, nullptr);
    EXPECT_EQ(RenderExecutionStatus::Completed, depthReport->status);
}

TEST_F(RenderPassValidationFixture,
       TypedShadowPassPublishesGraphOwnedOutputConsumedByOpaque)
{
    RVX_REQUIRE_RENDER_RUNTIME_PIPELINE();

    RenderGraph graph;
    graph.SetDevice(&device);

    const std::vector<RenderDrawItem> emptyDrawItems;
    const SceneMeshPassPreparation preparation = PrepareOpaquePackets(
        emptyDrawItems, emptyDrawItems);
    const RenderFramePlanCompileResult compiled = CompileForcedDirectPlan(
        preparation, 79);
    ASSERT_TRUE(compiled.succeeded);
    RenderFrameExecutionReport report = MakeExecutionReport(compiled.plan);

    RHITextureDesc colorDesc = RHITextureDesc::RenderTarget(
        64, 64, RHIFormat::RGBA8_UNORM);
    colorDesc.debugName = "TypedShadowOpaqueColor";

    RenderPassRecordContext context;
    context.view = view;
    context.view.renderGraph = &graph;
    context.view.viewCache = &viewCache;
    context.view.colorTarget = graph.CreateTexture(colorDesc);
    context.view.viewportWidth = 64;
    context.view.viewportHeight = 64;
    context.view.aspectRatio = 1.0f;
    context.view.fieldOfView = 1.0472f;
    context.view.nearPlane = 0.1f;
    context.view.farPlane = 100.0f;
    context.view.cameraPosition = Vec3(0.0f, 0.0f, 5.0f);
    context.view.cameraForward = Vec3(0.0f, 0.0f, -1.0f);
    context.view.inverseViewMatrix = Mat4Identity();
    graph.SetExportState(context.view.colorTarget, RHIResourceState::RenderTarget);
    context.view.renderFrameExecutionPlan = &compiled.plan;
    context.view.meshPassPreparation = &preparation;
    context.view.renderVisibility = nullptr;
    context.view.renderFrameExecutionReport = &report;
    context.identity.graph = &graph;
    context.identity.graphIdentity = graph.GetGraphIdentity();
    context.identity.graphRecordingGeneration = graph.GetRecordingGeneration();
    context.identity.frameSequence = compiled.plan.frameSequence;
    context.identity.viewOrdinal = compiled.plan.viewOrdinal;
    context.identity.recordEpoch = 11;
    context.executionPlan = &compiled.plan;
    context.meshPassPreparation = &preparation;
    context.visibility = nullptr;
    context.executionReport = &report;
    context.renderScene = &scene;
    context.opaqueDrawItems = &emptyDrawItems;
    context.maskedDrawItems = &emptyDrawItems;
    context.directionalShadow.identity = context.identity;
    context.rayTracedShadow.identity = context.identity;
    context.results = std::make_shared<RenderPassRecordResults>();
    context.results->identity = context.identity;
    context.results->directionalShadowOutput.identity = context.identity;
    context.frameSnapshot = MakeRenderPassFrameSnapshot(
        context, *context.results);

    ShadowPassConfig shadowConfig;
    shadowConfig.numCascades = 2;
    shadowConfig.shadowMapSize = 64;
    shadowConfig.cascadeBlendRatio = 0.1f;

    ShadowPass shadowPass;
    ConfigureResources(shadowPass, gpuResources, pipelineCache);
    shadowPass.SetConfig(shadowConfig);
    ConfigurePrimaryDirectionalLightRecord(
        context, Vec3{-0.3f, -1.0f, -0.2f}, Vec3{1.0f, 1.0f, 1.0f}, 1.0f);
    shadowPass.SetEnabled(true);
    shadowPass.AddToGraph(graph, context);

    const DirectionalShadowRecordOutput producedOutput =
        context.results->directionalShadowOutput;
    EXPECT_TRUE(producedOutput.enabled);
    EXPECT_TRUE(producedOutput.IsCompatibleWith(context.identity));
    EXPECT_TRUE(HasCurrentGraphProvenance(
        producedOutput.shadowMap, context.identity));
    EXPECT_EQ(static_cast<size_t>(shadowConfig.numCascades),
              producedOutput.cascadeViewProjections.size());
    EXPECT_EQ(producedOutput.cascadeViewProjections.size(),
              producedOutput.cascadeSplitDepths.size());
    EXPECT_EQ(shadowConfig.numCascades,
              context.results->shadowStats.configuredCascadeCount);
    EXPECT_EQ(shadowConfig.numCascades,
              context.results->shadowStats.declaredCascadeResourceCount);

    // Opaque receives the graph-owned value.  It has no persistent shadow
    // source, and later mutations of the producer cannot change this recording.
    context.directionalShadow = producedOutput;
    OpaquePass opaquePass;
    ConfigureResources(opaquePass, gpuResources, pipelineCache, materialSystem);
    opaquePass.AddToGraph(graph, context);
    shadowPass.SetEnabled(false);
    ShadowPassConfig mutatedConfig = shadowPass.GetConfig();
    mutatedConfig.numCascades = 1;
    mutatedConfig.shadowMapSize = 32;
    shadowPass.SetConfig(mutatedConfig);

    graph.Compile();
    ASSERT_TRUE(graph.GetCompileStats().compileValid);
    EXPECT_EQ(2u, graph.GetCompileStats().totalPasses);

    RecordingCommandContext commandContext;
    graph.Execute(commandContext);
    opaquePass.PublishRecordResults(context.results, context.identity);
    EXPECT_TRUE(opaquePass.GetShadowStats().requested);
    EXPECT_TRUE(opaquePass.GetShadowStats().renderGraphReadDeclared);
    EXPECT_TRUE(opaquePass.GetShadowStats().frameShadowReady);
}

TEST_F(RenderPassValidationFixture,
       TypedShadowPassFailClosedOutputStillAllowsOpaqueRegistration)
{
    RVX_REQUIRE_RENDER_RUNTIME_PIPELINE();

    const std::vector<RenderDrawItem> emptyDrawItems;
    const SceneMeshPassPreparation preparation = PrepareOpaquePackets(
        emptyDrawItems, emptyDrawItems);
    const RenderFramePlanCompileResult compiled = CompileForcedDirectPlan(
        preparation, 80);
    ASSERT_TRUE(compiled.succeeded);

    const auto makeContext = [this,
                              &compiled,
                              &preparation,
                              &emptyDrawItems](RenderGraph& graph,
                                               RenderFrameExecutionReport& report)
    {
        RHITextureDesc colorDesc = RHITextureDesc::RenderTarget(
            32, 32, RHIFormat::RGBA8_UNORM);
        RenderPassRecordContext context;
        context.view = view;
        context.view.renderGraph = &graph;
        context.view.viewCache = &viewCache;
        context.view.colorTarget = graph.CreateTexture(colorDesc);
        context.view.viewportWidth = 32;
        context.view.viewportHeight = 32;
        graph.SetExportState(context.view.colorTarget,
                             RHIResourceState::RenderTarget);
        context.view.renderFrameExecutionPlan = &compiled.plan;
        context.view.meshPassPreparation = &preparation;
        context.view.renderVisibility = nullptr;
        context.view.renderFrameExecutionReport = &report;
        context.identity.graph = &graph;
        context.identity.graphIdentity = graph.GetGraphIdentity();
        context.identity.graphRecordingGeneration = graph.GetRecordingGeneration();
        context.identity.frameSequence = compiled.plan.frameSequence;
        context.identity.viewOrdinal = compiled.plan.viewOrdinal;
        context.identity.recordEpoch = 12;
        context.executionPlan = &compiled.plan;
        context.meshPassPreparation = &preparation;
        context.visibility = nullptr;
        context.executionReport = &report;
        context.renderScene = &scene;
        context.opaqueDrawItems = &emptyDrawItems;
        context.maskedDrawItems = &emptyDrawItems;
        context.directionalShadow.identity = context.identity;
        context.rayTracedShadow.identity = context.identity;
        context.results = std::make_shared<RenderPassRecordResults>();
        context.results->identity = context.identity;
        context.results->directionalShadowOutput.identity = context.identity;
        context.frameSnapshot = MakeRenderPassFrameSnapshot(
            context, *context.results);
        return context;
    };

    const auto exerciseFailClosedProducer = [this, &makeContext](
                                               const char* caseName,
                                               const auto& configureShadow,
                                               bool invalidateContext,
                                               bool selectNonCasterPrimary)
    {
        SCOPED_TRACE(caseName);
        RenderGraph graph;
        graph.SetDevice(&device);
        RenderFrameExecutionReport report;
        report.frameSequence = 80;
        RenderPassRecordContext context = makeContext(graph, report);

        RenderScene selectionScene;
        if (selectNonCasterPrimary)
        {
            RenderLight firstDirectional;
            firstDirectional.type = RenderLight::Type::Directional;
            firstDirectional.direction = Vec3{0.1f, -0.9f, 0.2f};
            firstDirectional.intensity = 2.0f;
            firstDirectional.castsShadow = false;
            selectionScene.AddLight(firstDirectional);

            RenderLight laterCaster = firstDirectional;
            laterCaster.direction = Vec3{-0.6f, -0.4f, 0.7f};
            laterCaster.intensity = 9.0f;
            laterCaster.castsShadow = true;
            selectionScene.AddLight(laterCaster);

            // The record keeps the first positive directional light even
            // though a later directional light casts shadows.
            context.renderScene = &selectionScene;
            context.primaryDirectionalLight =
                SelectPrimaryDirectionalLightRecordInput(selectionScene);
            context.frameSnapshot = MakeRenderPassFrameSnapshot(
                context, *context.results);
            ASSERT_TRUE(context.primaryDirectionalLight.selected);
            ASSERT_FALSE(context.primaryDirectionalLight.castsShadow);
            ASSERT_FALSE(context.primaryDirectionalLight.IsShadowEligible());
            ASSERT_TRUE(context.frameSnapshot);
            EXPECT_FALSE(
                context.frameSnapshot->primaryDirectionalLight.castsShadow);
        }

        ShadowPass shadowPass;
        configureShadow(shadowPass);
        if (invalidateContext)
        {
            context.legacyAdapter = true;
        }
        shadowPass.AddToGraph(graph, context);

        EXPECT_FALSE(context.results->directionalShadowOutput.enabled);
        EXPECT_TRUE(context.results->directionalShadowOutput.IsCompatibleWith(
            context.identity));
        EXPECT_EQ(0u, context.results->shadowStats.configuredCascadeCount);
        EXPECT_EQ(0u, context.results->shadowStats.declaredCascadeResourceCount);
        EXPECT_EQ(0u, context.results->shadowStats.resolvedCascadeViewCount);
        EXPECT_EQ(0u, context.results->shadowStats.drawCount);

        // Only the producer is invalid.  Its current disabled output remains a
        // valid input for a separately validated Opaque registration.
        context.legacyAdapter = false;
        context.directionalShadow = context.results->directionalShadowOutput;
        OpaquePass opaquePass;
        ConfigureResources(opaquePass, gpuResources, pipelineCache, materialSystem);
        opaquePass.AddToGraph(graph, context);
        graph.Compile();
        EXPECT_TRUE(graph.GetCompileStats().compileValid);

        if (selectNonCasterPrimary)
        {
            const RenderGraph::Diagnostics diagnostics = graph.GetDiagnostics();
            const auto shadowDiagnostic = std::find_if(
                diagnostics.passes.begin(), diagnostics.passes.end(),
                [](const RenderGraph::PassDiagnostic& diagnostic)
                {
                    return diagnostic.name == "ShadowPass";
                });
            ASSERT_NE(diagnostics.passes.end(), shadowDiagnostic);
            EXPECT_TRUE(shadowDiagnostic->usages.empty());

            RecordingCommandContext commands;
            graph.Execute(commands);
            // The only render pass belongs to the independently registered
            // Opaque pass.  ShadowPass declared no resource usage above, so
            // it has no render pass to execute for this record.
            EXPECT_EQ(1u, commands.beginRenderPassCount);
            EXPECT_EQ(1u, commands.endRenderPassCount);
        }
    };

    exerciseFailClosedProducer(
        "disabled",
        [this](ShadowPass& shadowPass)
        {
            ConfigureResources(shadowPass, gpuResources, pipelineCache);
        },
        false,
        false);
    exerciseFailClosedProducer(
        "unsupported",
        [](ShadowPass& shadowPass)
        {
            shadowPass.SetEnabled(true);
        },
        false,
        false);
    exerciseFailClosedProducer(
        "invalid context",
        [this](ShadowPass& shadowPass)
        {
            ConfigureResources(shadowPass, gpuResources, pipelineCache);
            shadowPass.SetEnabled(true);
        },
        true,
        false);
    exerciseFailClosedProducer(
        "selected non-caster primary",
        [this](ShadowPass& shadowPass)
        {
            ConfigureResources(shadowPass, gpuResources, pipelineCache);
            shadowPass.SetEnabled(true);
        },
        false,
        true);
}

TEST_F(RenderPassValidationFixture,
       TypedShadowPassIsolatesOutputsAcrossInverseExecutedGraphs)
{
    RVX_REQUIRE_RENDER_RUNTIME_PIPELINE();

    RenderGraph graphA;
    RenderGraph graphB;
    graphA.SetDevice(&device);
    graphB.SetDevice(&device);

    const std::vector<RenderDrawItem> emptyDrawItems;
    const SceneMeshPassPreparation preparationA = PrepareOpaquePackets(
        emptyDrawItems, emptyDrawItems);
    const SceneMeshPassPreparation preparationB = PrepareOpaquePackets(
        emptyDrawItems, emptyDrawItems);
    const RenderFramePlanCompileResult compiledA = CompileForcedDirectPlan(
        preparationA, 81);
    const RenderFramePlanCompileResult compiledB = CompileForcedDirectPlan(
        preparationB, 82);
    ASSERT_TRUE(compiledA.succeeded);
    ASSERT_TRUE(compiledB.succeeded);
    RenderFrameExecutionReport reportA = MakeExecutionReport(compiledA.plan);
    RenderFrameExecutionReport reportB = MakeExecutionReport(compiledB.plan);

    const auto makeContext = [this, &emptyDrawItems](
                                 RenderGraph& graph,
                                 const RenderFrameExecutionPlan& plan,
                                 const SceneMeshPassPreparation& preparation,
                                 RenderFrameExecutionReport& report,
                                 uint64 recordEpoch,
                                 const char* colorTargetName)
    {
        RHITextureDesc colorDesc = RHITextureDesc::RenderTarget(
            64, 64, RHIFormat::RGBA8_UNORM);
        colorDesc.debugName = colorTargetName;

        RenderPassRecordContext context;
        context.view = view;
        context.view.renderGraph = &graph;
        context.view.viewCache = &viewCache;
        context.view.colorTarget = graph.CreateTexture(colorDesc);
        context.view.viewportWidth = 64;
        context.view.viewportHeight = 64;
        context.view.aspectRatio = 1.0f;
        context.view.fieldOfView = 1.0472f;
        context.view.nearPlane = 0.1f;
        context.view.farPlane = 100.0f;
        context.view.cameraPosition = Vec3(0.0f, 0.0f, 5.0f);
        context.view.cameraForward = Vec3(0.0f, 0.0f, -1.0f);
        context.view.inverseViewMatrix = Mat4Identity();
        graph.SetExportState(context.view.colorTarget,
                             RHIResourceState::RenderTarget);
        context.view.renderFrameExecutionPlan = &plan;
        context.view.meshPassPreparation = &preparation;
        context.view.renderVisibility = nullptr;
        context.view.renderFrameExecutionReport = &report;
        context.identity.graph = &graph;
        context.identity.graphIdentity = graph.GetGraphIdentity();
        context.identity.graphRecordingGeneration = graph.GetRecordingGeneration();
        context.identity.frameSequence = plan.frameSequence;
        context.identity.viewOrdinal = plan.viewOrdinal;
        context.identity.recordEpoch = recordEpoch;
        context.executionPlan = &plan;
        context.meshPassPreparation = &preparation;
        context.visibility = nullptr;
        context.executionReport = &report;
        context.renderScene = &scene;
        context.opaqueDrawItems = &emptyDrawItems;
        context.maskedDrawItems = &emptyDrawItems;
        context.directionalShadow.identity = context.identity;
        context.rayTracedShadow.identity = context.identity;
        context.results = std::make_shared<RenderPassRecordResults>();
        context.results->identity = context.identity;
        context.results->directionalShadowOutput.identity = context.identity;
        context.frameSnapshot = MakeRenderPassFrameSnapshot(
            context, *context.results);
        return context;
    };

    RenderPassRecordContext contextA = makeContext(
        graphA,
        compiledA.plan,
        preparationA,
        reportA,
        21,
        "TypedShadowIsolationColorA");
    RenderPassRecordContext contextB = makeContext(
        graphB,
        compiledB.plan,
        preparationB,
        reportB,
        22,
        "TypedShadowIsolationColorB");

    ShadowPass shadowPass;
    ConfigureResources(shadowPass, gpuResources, pipelineCache);

    ShadowPassConfig configA;
    configA.numCascades = 2;
    configA.shadowMapSize = 64;
    configA.cascadeBlendRatio = 0.05f;
    shadowPass.SetConfig(configA);
    ConfigurePrimaryDirectionalLightRecord(
        contextA, Vec3{-0.3f, -1.0f, -0.2f}, Vec3{1.0f, 1.0f, 1.0f}, 1.0f);
    shadowPass.SetEnabled(true);
    shadowPass.AddToGraph(graphA, contextA);
    const DirectionalShadowRecordOutput outputA =
        contextA.results->directionalShadowOutput;
    contextA.primaryDirectionalLight.direction = Vec3{0.0f, 1.0f, 0.0f};
    contextA.view.directionalLightDirection = Vec3{0.0f, 1.0f, 0.0f};
    ASSERT_TRUE(contextA.frameSnapshot);
    EXPECT_FLOAT_EQ(contextA.frameSnapshot->primaryDirectionalLight.direction.x, -0.3f);
    EXPECT_FLOAT_EQ(contextA.frameSnapshot->view.directionalLightDirection.y, -1.0f);

    ShadowPassConfig configB;
    configB.numCascades = 3;
    configB.shadowMapSize = 96;
    configB.cascadeBlendRatio = 0.2f;
    shadowPass.SetConfig(configB);
    ConfigurePrimaryDirectionalLightRecord(
        contextB, Vec3{0.6f, -0.1f, 0.9f}, Vec3{0.5f, 0.7f, 1.0f}, 3.0f);
    shadowPass.AddToGraph(graphB, contextB);
    const DirectionalShadowRecordOutput outputB =
        contextB.results->directionalShadowOutput;

    ASSERT_TRUE(contextB.frameSnapshot);
    EXPECT_FLOAT_EQ(contextB.frameSnapshot->primaryDirectionalLight.direction.x, 0.6f);
    EXPECT_FLOAT_EQ(contextB.frameSnapshot->view.directionalLightColor.y, 0.7f);

    ASSERT_TRUE(outputA.enabled);
    ASSERT_TRUE(outputB.enabled);
    EXPECT_TRUE(outputA.IsCompatibleWith(contextA.identity));
    EXPECT_TRUE(outputB.IsCompatibleWith(contextB.identity));
    EXPECT_FALSE(outputA.IsCompatibleWith(contextB.identity));
    EXPECT_FALSE(outputB.IsCompatibleWith(contextA.identity));
    EXPECT_TRUE(HasCurrentGraphProvenance(outputA.shadowMap, contextA.identity));
    EXPECT_TRUE(HasCurrentGraphProvenance(outputB.shadowMap, contextB.identity));
    EXPECT_FALSE(HasCurrentGraphProvenance(outputA.shadowMap, contextB.identity));
    EXPECT_FALSE(HasCurrentGraphProvenance(outputB.shadowMap, contextA.identity));
    EXPECT_EQ(configA.shadowMapSize, outputA.shadowMapSize);
    EXPECT_EQ(configB.shadowMapSize, outputB.shadowMapSize);
    EXPECT_EQ(configA.cascadeBlendRatio, outputA.cascadeBlendRatio);
    EXPECT_EQ(configB.cascadeBlendRatio, outputB.cascadeBlendRatio);
    ASSERT_EQ(static_cast<size_t>(configA.numCascades),
              outputA.cascadeViewProjections.size());
    ASSERT_EQ(static_cast<size_t>(configB.numCascades),
              outputB.cascadeViewProjections.size());
    EXPECT_NE(outputA.cascadeViewProjections[0][0][0],
              outputB.cascadeViewProjections[0][0][0]);

    contextA.directionalShadow = outputA;
    contextB.directionalShadow = outputB;
    OpaquePass opaquePassA;
    OpaquePass opaquePassB;
    ConfigureResources(opaquePassA, gpuResources, pipelineCache, materialSystem);
    ConfigureResources(opaquePassB, gpuResources, pipelineCache, materialSystem);
    opaquePassA.AddToGraph(graphA, contextA);
    opaquePassB.AddToGraph(graphB, contextB);

    // Both registrations above own their config/light snapshots.  A further
    // persistent mutation must affect neither graph, even when B executes first.
    ShadowPassConfig mutatedConfig = shadowPass.GetConfig();
    mutatedConfig.numCascades = 1;
    mutatedConfig.shadowMapSize = 32;
    shadowPass.SetConfig(mutatedConfig);
    shadowPass.SetEnabled(false);

    graphB.Compile();
    graphA.Compile();
    ASSERT_TRUE(graphB.GetCompileStats().compileValid);
    ASSERT_TRUE(graphA.GetCompileStats().compileValid);

    RecordingCommandContext commandContextB;
    RecordingCommandContext commandContextA;
    graphB.Execute(commandContextB);
    graphA.Execute(commandContextA);

    EXPECT_EQ(configB.numCascades,
              contextB.results->shadowStats.configuredCascadeCount);
    EXPECT_EQ(configB.numCascades,
              contextB.results->shadowStats.declaredCascadeResourceCount);
    EXPECT_EQ(configB.numCascades,
              contextB.results->shadowStats.resolvedCascadeViewCount);
    EXPECT_EQ(configB.numCascades, contextB.results->shadowStats.drawCount);
    EXPECT_EQ(configA.numCascades,
              contextA.results->shadowStats.configuredCascadeCount);
    EXPECT_EQ(configA.numCascades,
              contextA.results->shadowStats.declaredCascadeResourceCount);
    EXPECT_EQ(configA.numCascades,
              contextA.results->shadowStats.resolvedCascadeViewCount);
    EXPECT_EQ(configA.numCascades, contextA.results->shadowStats.drawCount);

    opaquePassB.PublishRecordResults(contextB.results, contextB.identity);
    opaquePassA.PublishRecordResults(contextA.results, contextA.identity);
    EXPECT_TRUE(opaquePassB.GetShadowStats().renderGraphReadDeclared);
    EXPECT_TRUE(opaquePassB.GetShadowStats().frameShadowReady);
    EXPECT_TRUE(opaquePassA.GetShadowStats().renderGraphReadDeclared);
    EXPECT_TRUE(opaquePassA.GetShadowStats().frameShadowReady);

    const RenderPassRecordIdentity oldIdentityA = contextA.identity;
    graphA.Clear();
    RenderPassRecordIdentity refreshedIdentityA = oldIdentityA;
    refreshedIdentityA.graphIdentity = graphA.GetGraphIdentity();
    refreshedIdentityA.graphRecordingGeneration = graphA.GetRecordingGeneration();
    ++refreshedIdentityA.recordEpoch;
    EXPECT_FALSE(oldIdentityA.Matches(graphA));
    EXPECT_FALSE(outputA.IsCompatibleWith(refreshedIdentityA));
    EXPECT_FALSE(HasCurrentGraphProvenance(outputA.shadowMap,
                                           refreshedIdentityA));
}

TEST(RenderPassValidation, RayTracingPipelineValidationRejectsInvalidShaderGroupContracts)
{
    const uint8 shaderBytecode[] = {0x52, 0x56, 0x58, 0x00};
    auto makeShader = [&](RHIShaderStage stage) -> std::unique_ptr<FakeShader>
    {
        RHIShaderDesc desc;
        desc.stage = stage;
        desc.bytecode = shaderBytecode;
        desc.bytecodeSize = sizeof(shaderBytecode);
        return std::make_unique<FakeShader>(desc);
    };

    const std::unique_ptr<FakeShader> rayGenerationShader = makeShader(RHIShaderStage::RayGeneration);
    const std::unique_ptr<FakeShader> missShader = makeShader(RHIShaderStage::Miss);
    const std::unique_ptr<FakeShader> closestHitShader = makeShader(RHIShaderStage::ClosestHit);
    const std::unique_ptr<FakeShader> anyHitShader = makeShader(RHIShaderStage::AnyHit);
    const std::unique_ptr<FakeShader> intersectionShader = makeShader(RHIShaderStage::Intersection);
    const std::unique_ptr<FakeShader> pixelShader = makeShader(RHIShaderStage::Pixel);

    FakePipelineLayout layout;

    RHIRayTracingShaderGroupDesc rayGenerationGroup;
    rayGenerationGroup.type = RHIRayTracingShaderGroupType::General;
    rayGenerationGroup.generalShader = rayGenerationShader.get();

    RHIRayTracingPipelineDesc baseDesc;
    baseDesc.pipelineLayout = &layout;
    baseDesc.shaderGroups.push_back(rayGenerationGroup);
    baseDesc.maxRecursionDepth = 1;
    baseDesc.maxPayloadSize = sizeof(uint32);
    baseDesc.maxAttributeSize = sizeof(float) * 2;
    EXPECT_TRUE(ValidateRHIRayTracingPipelineDesc(baseDesc));

    RHIRayTracingPipelineDesc oversizedAttributeDesc = baseDesc;
    oversizedAttributeDesc.maxAttributeSize = RVX_RAY_TRACING_MAX_ATTRIBUTE_SIZE + 1u;
    RHIRayTracingValidationResult result = ValidateRHIRayTracingPipelineDesc(oversizedAttributeDesc);
    EXPECT_FALSE(result.valid);
    EXPECT_STREQ(result.message, "ray tracing pipeline max attribute size exceeds the native 32-byte limit");

    RHIRayTracingPipelineDesc missingRayGenDesc = baseDesc;
    missingRayGenDesc.shaderGroups[0].generalShader = missShader.get();
    result = ValidateRHIRayTracingPipelineDesc(missingRayGenDesc);
    EXPECT_FALSE(result.valid);
    EXPECT_STREQ(result.message, "ray tracing pipeline requires a ray-generation shader group");

    RHIRayTracingPipelineDesc pollutedGeneralDesc = baseDesc;
    pollutedGeneralDesc.shaderGroups[0].closestHitShader = closestHitShader.get();
    result = ValidateRHIRayTracingPipelineDesc(pollutedGeneralDesc);
    EXPECT_FALSE(result.valid);
    EXPECT_STREQ(result.message, "general shader group cannot include hit shaders");

    RHIRayTracingShaderGroupDesc triangleGroup;
    triangleGroup.type = RHIRayTracingShaderGroupType::TrianglesHitGroup;
    triangleGroup.closestHitShader = closestHitShader.get();

    RHIRayTracingPipelineDesc triangleDesc = baseDesc;
    triangleDesc.shaderGroups.push_back(triangleGroup);
    EXPECT_TRUE(ValidateRHIRayTracingPipelineDesc(triangleDesc));

    RHIRayTracingPipelineDesc invalidTriangleDesc = triangleDesc;
    invalidTriangleDesc.shaderGroups[1].intersectionShader = intersectionShader.get();
    result = ValidateRHIRayTracingPipelineDesc(invalidTriangleDesc);
    EXPECT_FALSE(result.valid);
    EXPECT_STREQ(result.message, "triangle hit group cannot include general or intersection shaders");

    invalidTriangleDesc = triangleDesc;
    invalidTriangleDesc.shaderGroups[1].closestHitShader = pixelShader.get();
    result = ValidateRHIRayTracingPipelineDesc(invalidTriangleDesc);
    EXPECT_FALSE(result.valid);
    EXPECT_STREQ(result.message, "triangle hit group closest-hit shader has the wrong stage");

    RHIRayTracingShaderGroupDesc proceduralGroup;
    proceduralGroup.type = RHIRayTracingShaderGroupType::ProceduralHitGroup;
    proceduralGroup.intersectionShader = intersectionShader.get();
    proceduralGroup.anyHitShader = anyHitShader.get();

    RHIRayTracingPipelineDesc proceduralDesc = baseDesc;
    proceduralDesc.shaderGroups.push_back(proceduralGroup);
    EXPECT_TRUE(ValidateRHIRayTracingPipelineDesc(proceduralDesc));

    RHIRayTracingPipelineDesc invalidProceduralDesc = proceduralDesc;
    invalidProceduralDesc.shaderGroups[1].generalShader = rayGenerationShader.get();
    result = ValidateRHIRayTracingPipelineDesc(invalidProceduralDesc);
    EXPECT_FALSE(result.valid);
    EXPECT_STREQ(result.message, "procedural hit group cannot include a general shader");

    invalidProceduralDesc = proceduralDesc;
    invalidProceduralDesc.shaderGroups[1].anyHitShader = pixelShader.get();
    result = ValidateRHIRayTracingPipelineDesc(invalidProceduralDesc);
    EXPECT_FALSE(result.valid);
    EXPECT_STREQ(result.message, "procedural hit group any-hit shader has the wrong stage");

    invalidProceduralDesc = proceduralDesc;
    invalidProceduralDesc.shaderGroups[1].type = static_cast<RHIRayTracingShaderGroupType>(0xFFu);
    result = ValidateRHIRayTracingPipelineDesc(invalidProceduralDesc);
    EXPECT_FALSE(result.valid);
    EXPECT_STREQ(result.message, "ray tracing pipeline shader group has an invalid type");

    RHIRayTracingPipelineDesc emptyExportNameDesc = baseDesc;
    emptyExportNameDesc.shaderGroups[0].exportName = "";
    result = ValidateRHIRayTracingPipelineDesc(emptyExportNameDesc);
    EXPECT_FALSE(result.valid);
    EXPECT_STREQ(result.message, "ray tracing pipeline shader group export name cannot be empty");

    RHIRayTracingPipelineDesc duplicateExportDesc = triangleDesc;
    duplicateExportDesc.shaderGroups[0].exportName = "SharedGroup";
    duplicateExportDesc.shaderGroups[1].exportName = "SharedGroup";
    result = ValidateRHIRayTracingPipelineDesc(duplicateExportDesc);
    EXPECT_FALSE(result.valid);
    EXPECT_STREQ(result.message, "ray tracing pipeline shader exports must be unique");

    RHIRayTracingPipelineDesc importCollisionDesc = triangleDesc;
    importCollisionDesc.shaderGroups[0].exportName = "ShadowHit_ClosestHit";
    importCollisionDesc.shaderGroups[1].exportName = "ShadowHit";
    result = ValidateRHIRayTracingPipelineDesc(importCollisionDesc);
    EXPECT_FALSE(result.valid);
    EXPECT_STREQ(result.message, "ray tracing pipeline shader exports must be unique");
}

TEST_F(RenderPassValidationFixture, ShadowPassReportsSupportedWithDepthPipelineAndResources)
{
    RVX_REQUIRE_RENDER_RUNTIME_PIPELINE();

    ShadowPass pass;
    ConfigureResources(pass, gpuResources, pipelineCache);
    pass.SetEnabled(true);

    EXPECT_TRUE(pass.IsRequestedEnabled());
    EXPECT_TRUE(pass.IsSupported()) << pass.GetUnsupportedReason();
    EXPECT_TRUE(pass.IsEnabled());

    pipelineCache.m_pipelineCache.clear();
    pipelineCache.m_depthOnlyVertexShader.Reset();
    EXPECT_FALSE(pass.IsSupported());
    EXPECT_FALSE(pass.GetUnsupportedReason().empty());
}

TEST_F(RenderPassValidationFixture, RayTracedShadowPassRuntimeCreatesDescriptorSetAndDispatchesRays)
{
    RVX_REQUIRE_RENDER_RUNTIME_PIPELINE(true, true);

    RayTracingSceneManager rayTracingScene;
    ASSERT_NO_FATAL_FAILURE(PrepareRayTracingSceneForSingleObject(device, gpuResources, scene, rayTracingScene));

    RenderGraph graph;
    graph.SetDevice(&device);

    RHITextureDesc depthDesc = RHITextureDesc::DepthStencil(64, 64, PipelineCache::GetDefaultDepthStencilFormat());
    depthDesc.debugName = "RayTracedShadowRuntimeDepth";
    RHITextureRef depthTexture = device.CreateTexture(depthDesc);
    ASSERT_TRUE(depthTexture);
    view.depthTarget = graph.ImportTexture(depthTexture.Get(), RHIResourceState::DepthRead);

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
    view.viewMatrix = Mat4Identity();
    view.projectionMatrix = Mat4Identity();
    view.viewProjectionMatrix = Mat4Identity();
    view.inverseViewMatrix = Mat4Identity();
    view.inverseProjectionMatrix = Mat4Identity();
    view.previousViewProjectionMatrix = Mat4Identity();
    view.resetTemporalHistory = false;
    view.frameNumber = 99;

    RayTracedShadowPass pass;
    pass.SetEnabled(true);
    pass.OnAdd(&device);
    ConfigureResources(pass, gpuResources, pipelineCache, viewCache);
    pass.SetRayTracingScene(&rayTracingScene);

    ASSERT_TRUE(pass.IsSupported()) << pass.GetUnsupportedReason();

    pass.AddToGraph(graph, view);
    graph.Compile();
    EXPECT_TRUE(graph.GetCompileStats().compileValid);
    EXPECT_EQ(graph.GetCompileStats().totalPasses, 1u);

    RecordingCommandContext ctx;
    graph.Execute(ctx);

    EXPECT_EQ(ctx.dispatchRaysCount, 1u);
    EXPECT_TRUE(ctx.lastDispatchRaysValidation.valid) << ctx.lastDispatchRaysValidation.message;
    EXPECT_EQ(ctx.lastDispatchRaysDesc.width, 64u);
    EXPECT_EQ(ctx.lastDispatchRaysDesc.height, 64u);
    ASSERT_FALSE(ctx.pipelineSequence.empty());
    EXPECT_EQ(ctx.pipelineSequence.back(), pipelineCache.GetRayTracedShadowPipeline());
    EXPECT_TRUE(std::any_of(ctx.descriptorSetSequence.begin(), ctx.descriptorSetSequence.end(),
                            [](uint32 set) { return set == 0; }));
    ExpectCommandBefore(ctx, "SetPipeline", "DispatchRays");
    ExpectCommandBefore(ctx, "SetDescriptorSet", "DispatchRays");

    pass.NotifySubmission(GPUCompletionToken{});
    const RayTracedShadowPassStats& stats = pass.GetStats();
    EXPECT_TRUE(stats.outputDeclared);
    EXPECT_TRUE(stats.resourceViewsAvailable);
    EXPECT_TRUE(stats.descriptorSetAvailable);
    EXPECT_TRUE(stats.constantsUploaded);
    EXPECT_TRUE(stats.dispatchRecorded);
    EXPECT_FALSE(stats.historyAvailable);
    EXPECT_EQ(stats.dispatchPixelCount, 64u * 64u);

    // A device/pass re-add starts a new identity timeline.  The first
    // recording commonly restarts at frame 1 and must not be rejected by a
    // watermark from the retired device lifetime.
    pass.OnRemove();
    pass.SetEnabled(true);
    pass.OnAdd(&device);
    ConfigureResources(pass, gpuResources, pipelineCache, viewCache);
    pass.SetRayTracingScene(&rayTracingScene);

    RenderGraph restartedGraph;
    restartedGraph.SetDevice(&device);
    RHITextureRef restartedDepth = device.CreateTexture(depthDesc);
    ASSERT_TRUE(restartedDepth);
    ViewData restartedView = view;
    restartedView.renderGraph = &restartedGraph;
    restartedView.depthTarget = restartedGraph.ImportTexture(
        restartedDepth.Get(), RHIResourceState::DepthRead);
    restartedView.frameNumber = 1;
    pass.AddToGraph(restartedGraph, restartedView);
    restartedGraph.Compile();
    ASSERT_TRUE(restartedGraph.GetCompileStats().compileValid);
    RecordingCommandContext restartedContext;
    restartedGraph.Execute(restartedContext);
    ASSERT_EQ(1u, restartedContext.dispatchRaysCount);
    pass.NotifySubmission(GPUCompletionToken{});
    EXPECT_TRUE(pass.GetStats().dispatchRecorded);
}

TEST_F(RenderPassValidationFixture,
       RayTracedShadowPassNoEligiblePrimaryPublishesDisabledOutputWithoutWrites)
{
    RVX_REQUIRE_RENDER_RUNTIME_PIPELINE(true, true);

    RayTracingSceneManager rayTracingScene;
    ASSERT_NO_FATAL_FAILURE(PrepareRayTracingSceneForSingleObject(
        device, gpuResources, scene, rayTracingScene));

    RayTracedShadowPass pass;
    pass.SetEnabled(true);
    pass.OnAdd(&device);
    ConfigureResources(pass, gpuResources, pipelineCache, viewCache);
    pass.SetRayTracingScene(&rayTracingScene);
    ASSERT_TRUE(pass.IsSupported()) << pass.GetUnsupportedReason();

    ExpectRayTracedShadowRecordGateNoWork(
        device, viewCache, pass, "feature enabled with a selected non-caster",
        true, true, false, 420);
}

TEST_F(RenderPassValidationFixture,
       RayTracedShadowPassFeatureDisabledPublishesDisabledOutputWithoutWrites)
{
    RVX_REQUIRE_RENDER_RUNTIME_PIPELINE(true, true);

    RayTracingSceneManager rayTracingScene;
    ASSERT_NO_FATAL_FAILURE(PrepareRayTracingSceneForSingleObject(
        device, gpuResources, scene, rayTracingScene));

    RayTracedShadowPass pass;
    pass.SetEnabled(true);
    pass.OnAdd(&device);
    ConfigureResources(pass, gpuResources, pipelineCache, viewCache);
    pass.SetRayTracingScene(&rayTracingScene);
    ASSERT_TRUE(pass.IsSupported()) << pass.GetUnsupportedReason();

    ExpectRayTracedShadowRecordGateNoWork(
        device, viewCache, pass, "feature disabled with an eligible primary",
        false, true, true, 421);
}

TEST_F(RenderPassValidationFixture, RayTracedShadowPassRejectsOversizedMaterialTextureTable)
{
    RVX_REQUIRE_RENDER_RUNTIME_PIPELINE(true, true);

    constexpr uint32 kMaterialTextureLimit = RTShadowBindings::RVX_RT_SHADOW_MAX_MATERIAL_TEXTURES;
    constexpr uint32 kTextureCount = kMaterialTextureLimit + 1;

    RenderScene oversizedScene;
    std::vector<std::unique_ptr<Resource::MaterialResource>> materials;
    std::vector<Resource::TextureHandle> textures;
    std::vector<uint32_t> visibleObjectIndices;
    materials.reserve(kTextureCount);
    textures.reserve(kTextureCount);
    visibleObjectIndices.reserve(kTextureCount);

    for (uint32 i = 0; i < kTextureCount; ++i)
    {
        Resource::TextureHandle albedo = CreateTextureResource(11000 + i);
        albedo->SetName("RayTracedShadowMaterialOverflowTexture");
        gpuResources.UploadImmediate(albedo.Get());
        ASSERT_TRUE(gpuResources.IsGPUReady(albedo.GetId()));

        auto material = std::make_unique<Resource::MaterialResource>();
        material->SetId(12000 + i);
        material->SetName("RayTracedShadowMaterialOverflowMaterial");
        auto materialData = std::make_shared<Material>("RayTracedShadowMaterialOverflowMaterial");
        materialData->SetBaseColorTexture(
            TextureInfo("shadow_material_overflow_albedo_" + std::to_string(i) + ".png", 0));
        material->SetMaterialData(materialData);
        material->SetTexture("albedo", albedo);
        ASSERT_TRUE(gpuResources.UploadImmediate(material.get()));

        RenderObject object = MakeRenderObject(*meshResource, gpuResources);
        object.entityId = 13000 + i;
        object.material = gpuResources.GetHandle(material->GetId());
        oversizedScene.AddObject(object);
        visibleObjectIndices.push_back(i);

        textures.push_back(albedo);
        materials.push_back(std::move(material));
    }

    RayTracingSceneBuildPlan plan = BuildRayTracingSceneBuildPlan(
        oversizedScene,
        visibleObjectIndices,
        gpuResources.GetRegistry());
    ASSERT_TRUE(plan.HasWork());
    ASSERT_EQ(plan.instances.size(), kTextureCount);

    RayTracingSceneManager rayTracingScene;
    rayTracingScene.Initialize(&device);
    ASSERT_TRUE(rayTracingScene.IsSupported());
    ASSERT_TRUE(rayTracingScene.Prepare(plan)) << rayTracingScene.GetStats().fallbackReason;
    ASSERT_EQ(rayTracingScene.GetInstanceMaterialTextureTable().size(), kTextureCount);

    RayTracedShadowPass pass;
    pass.SetEnabled(true);
    pass.OnAdd(&device);
    ConfigureResources(pass, gpuResources, pipelineCache, viewCache);
    pass.SetRayTracingScene(&rayTracingScene);

    EXPECT_FALSE(pass.IsSupported());
    EXPECT_EQ(pass.GetUnsupportedReason(),
              "Ray tracing material texture table exceeds the supported descriptor count");

    RenderGraph graph;
    graph.SetDevice(&device);
    RHITextureRef depthTexture = device.CreateTexture(RHITextureDesc::DepthStencil(
        4, 4, PipelineCache::GetDefaultDepthStencilFormat()));
    ASSERT_TRUE(depthTexture);
    ViewData overflowView;
    overflowView.renderGraph = &graph;
    overflowView.viewCache = &viewCache;
    overflowView.depthTarget = graph.ImportTexture(depthTexture.Get(), RHIResourceState::DepthRead);
    overflowView.viewportWidth = 4;
    overflowView.viewportHeight = 4;
    overflowView.frameNumber = 1;
    RenderPassRecordContext context = MakeRenderPassRecordContext(graph, overflowView);
    context.results = std::make_shared<RenderPassRecordResults>();
    context.frameSnapshot = MakeRenderPassFrameSnapshot(context, *context.results);
    pass.AddToGraph(graph, context);

    const RayTracedShadowPassStats& stats = context.results->rayTracedShadowStats;
    EXPECT_TRUE(stats.requested);
    EXPECT_FALSE(stats.supported);
    EXPECT_EQ(stats.materialTextureCount, kTextureCount);
    EXPECT_TRUE(stats.alphaTextureTableAvailable);
    EXPECT_TRUE(stats.alphaGeometryTableAvailable);
    EXPECT_FALSE(stats.materialTextureTableAvailable);
    EXPECT_FALSE(stats.outputDeclared);
    EXPECT_FALSE(stats.executionFailed);
    ASSERT_TRUE(context.results->rayTracedShadowOutput.executionState);
    EXPECT_FALSE(context.results->rayTracedShadowOutput.enabled);
    EXPECT_FALSE(context.results->rayTracedShadowOutput.executionState->dispatchReady.load());
    EXPECT_FALSE(context.results->rayTracedShadowOutput.executionState->executionFailed.load());
}

TEST_F(RenderPassValidationFixture, RayTracedShadowPassRejectsOversizedAlphaTextureTable)
{
    RVX_REQUIRE_RENDER_RUNTIME_PIPELINE(true, true);

    constexpr uint32 kAlphaTextureLimit = RTShadowBindings::RVX_RT_SHADOW_MAX_ALPHA_TEXTURES;
    constexpr uint32 kTextureCount = kAlphaTextureLimit + 1;

    RenderScene oversizedScene;
    std::vector<std::unique_ptr<Resource::MaterialResource>> materials;
    std::vector<Resource::TextureHandle> textures;
    std::vector<uint32_t> visibleObjectIndices;
    materials.reserve(kTextureCount);
    textures.reserve(kTextureCount);
    visibleObjectIndices.reserve(kTextureCount);

    for (uint32 i = 0; i < kTextureCount; ++i)
    {
        Resource::TextureHandle albedo = CreateTextureResource(14000 + i);
        albedo->SetName("RayTracedShadowAlphaOverflowTexture");
        gpuResources.UploadImmediate(albedo.Get());
        ASSERT_TRUE(gpuResources.IsGPUReady(albedo.GetId()));

        auto material = std::make_unique<Resource::MaterialResource>();
        material->SetId(15000 + i);
        material->SetName("RayTracedShadowAlphaOverflowMaterial");
        auto materialData = std::make_shared<Material>("RayTracedShadowAlphaOverflowMaterial");
        materialData->SetAlphaMode(Material::AlphaMode::Mask);
        materialData->SetBaseColorTexture(
            TextureInfo("shadow_alpha_overflow_albedo_" + std::to_string(i) + ".png", 0));
        material->SetMaterialData(materialData);
        material->SetTexture("albedo", albedo);
        ASSERT_TRUE(gpuResources.UploadImmediate(material.get()));

        RenderObject object = MakeRenderObject(*meshResource, gpuResources);
        object.entityId = 16000 + i;
        object.material = gpuResources.GetHandle(material->GetId());
        object.materialModes = {RenderMaterialMode::Masked};
        oversizedScene.AddObject(object);
        visibleObjectIndices.push_back(i);

        textures.push_back(albedo);
        materials.push_back(std::move(material));
    }

    RayTracingSceneBuildPlan plan = BuildRayTracingSceneBuildPlan(
        oversizedScene,
        visibleObjectIndices,
        gpuResources.GetRegistry());
    ASSERT_TRUE(plan.HasWork());
    ASSERT_EQ(plan.instances.size(), kTextureCount);

    RayTracingSceneManager rayTracingScene;
    rayTracingScene.Initialize(&device);
    ASSERT_TRUE(rayTracingScene.IsSupported());
    ASSERT_TRUE(rayTracingScene.Prepare(plan)) << rayTracingScene.GetStats().fallbackReason;
    ASSERT_EQ(rayTracingScene.GetInstanceAlphaTextureTable().size(), kTextureCount);

    RayTracedShadowPass pass;
    pass.SetEnabled(true);
    pass.OnAdd(&device);
    ConfigureResources(pass, gpuResources, pipelineCache, viewCache);
    pass.SetRayTracingScene(&rayTracingScene);

    EXPECT_FALSE(pass.IsSupported());
    EXPECT_EQ(pass.GetUnsupportedReason(),
              "Ray tracing alpha texture table exceeds the supported descriptor count");

    RenderGraph graph;
    graph.SetDevice(&device);
    RHITextureRef depthTexture = device.CreateTexture(RHITextureDesc::DepthStencil(
        4, 4, PipelineCache::GetDefaultDepthStencilFormat()));
    ASSERT_TRUE(depthTexture);
    ViewData overflowView;
    overflowView.renderGraph = &graph;
    overflowView.viewCache = &viewCache;
    overflowView.depthTarget = graph.ImportTexture(depthTexture.Get(), RHIResourceState::DepthRead);
    overflowView.viewportWidth = 4;
    overflowView.viewportHeight = 4;
    overflowView.frameNumber = 1;
    RenderPassRecordContext context = MakeRenderPassRecordContext(graph, overflowView);
    context.results = std::make_shared<RenderPassRecordResults>();
    context.frameSnapshot = MakeRenderPassFrameSnapshot(context, *context.results);
    pass.AddToGraph(graph, context);

    const RayTracedShadowPassStats& stats = context.results->rayTracedShadowStats;
    EXPECT_TRUE(stats.requested);
    EXPECT_FALSE(stats.supported);
    EXPECT_TRUE(stats.materialTextureTableAvailable);
    EXPECT_EQ(stats.alphaTextureCount, kTextureCount);
    EXPECT_TRUE(stats.alphaGeometryTableAvailable);
    EXPECT_FALSE(stats.alphaTextureTableAvailable);
    EXPECT_FALSE(stats.outputDeclared);
    EXPECT_FALSE(stats.executionFailed);
    ASSERT_TRUE(context.results->rayTracedShadowOutput.executionState);
    EXPECT_FALSE(context.results->rayTracedShadowOutput.enabled);
    EXPECT_FALSE(context.results->rayTracedShadowOutput.executionState->dispatchReady.load());
    EXPECT_FALSE(context.results->rayTracedShadowOutput.executionState->executionFailed.load());
}

TEST_F(RenderPassValidationFixture, RayTracedShadowPassRejectsOversizedAlphaGeometryBufferTable)
{
    RVX_REQUIRE_RENDER_RUNTIME_PIPELINE(true, true);

    constexpr uint32 kAlphaGeometryBufferLimit = RTShadowBindings::RVX_RT_SHADOW_MAX_ALPHA_GEOMETRY_BUFFERS;
    constexpr uint32 kMeshCount = kAlphaGeometryBufferLimit + 1;

    auto material = std::make_unique<Resource::MaterialResource>();
    material->SetId(17000);
    material->SetName("RayTracedShadowAlphaGeometryOverflowMaterial");
    auto materialData = std::make_shared<Material>("RayTracedShadowAlphaGeometryOverflowMaterial");
    materialData->SetAlphaMode(Material::AlphaMode::Mask);
    material->SetMaterialData(materialData);
    ASSERT_TRUE(gpuResources.UploadImmediate(material.get()));

    RenderScene oversizedScene;
    std::vector<std::unique_ptr<Resource::MeshResource>> meshes;
    std::vector<uint32_t> visibleObjectIndices;
    meshes.reserve(kMeshCount);
    visibleObjectIndices.reserve(kMeshCount);

    for (uint32 i = 0; i < kMeshCount; ++i)
    {
        auto mesh = CreateMeshResource(18000 + i);
        gpuResources.UploadImmediate(mesh.get());
        ASSERT_TRUE(gpuResources.IsGPUReady(mesh->GetId()));

        RenderObject object = MakeRenderObject(*mesh, gpuResources);
        object.entityId = 19000 + i;
        object.material = gpuResources.GetHandle(material->GetId());
        oversizedScene.AddObject(object);
        visibleObjectIndices.push_back(i);

        meshes.push_back(std::move(mesh));
    }

    RayTracingSceneBuildPlan plan = BuildRayTracingSceneBuildPlan(
        oversizedScene,
        visibleObjectIndices,
        gpuResources.GetRegistry());
    ASSERT_TRUE(plan.HasWork());
    ASSERT_EQ(plan.instances.size(), kMeshCount);
    ASSERT_EQ(plan.blasBuilds.size(), kMeshCount);

    RayTracingSceneManager rayTracingScene;
    rayTracingScene.Initialize(&device);
    ASSERT_TRUE(rayTracingScene.IsSupported());
    ASSERT_TRUE(rayTracingScene.Prepare(plan)) << rayTracingScene.GetStats().fallbackReason;
    ASSERT_EQ(rayTracingScene.GetInstanceAlphaIndexBufferTable().size(), kMeshCount);
    ASSERT_EQ(rayTracingScene.GetInstanceAlphaUVBufferTable().size(), kMeshCount);

    RayTracedShadowPass pass;
    pass.SetEnabled(true);
    pass.OnAdd(&device);
    ConfigureResources(pass, gpuResources, pipelineCache, viewCache);
    pass.SetRayTracingScene(&rayTracingScene);

    EXPECT_FALSE(pass.IsSupported());
    EXPECT_EQ(pass.GetUnsupportedReason(),
              "Ray tracing alpha geometry buffer table exceeds the supported descriptor count");

    RenderGraph graph;
    graph.SetDevice(&device);
    RHITextureRef depthTexture = device.CreateTexture(RHITextureDesc::DepthStencil(
        4, 4, PipelineCache::GetDefaultDepthStencilFormat()));
    ASSERT_TRUE(depthTexture);
    ViewData overflowView;
    overflowView.renderGraph = &graph;
    overflowView.viewCache = &viewCache;
    overflowView.depthTarget = graph.ImportTexture(depthTexture.Get(), RHIResourceState::DepthRead);
    overflowView.viewportWidth = 4;
    overflowView.viewportHeight = 4;
    overflowView.frameNumber = 1;
    RenderPassRecordContext context = MakeRenderPassRecordContext(graph, overflowView);
    context.results = std::make_shared<RenderPassRecordResults>();
    context.frameSnapshot = MakeRenderPassFrameSnapshot(context, *context.results);
    pass.AddToGraph(graph, context);

    const RayTracedShadowPassStats& stats = context.results->rayTracedShadowStats;
    EXPECT_TRUE(stats.requested);
    EXPECT_FALSE(stats.supported);
    EXPECT_TRUE(stats.materialTextureTableAvailable);
    EXPECT_TRUE(stats.alphaTextureTableAvailable);
    EXPECT_EQ(stats.alphaIndexBufferCount, kMeshCount);
    EXPECT_EQ(stats.alphaUVBufferCount, kMeshCount);
    EXPECT_FALSE(stats.alphaGeometryTableAvailable);
    EXPECT_FALSE(stats.outputDeclared);
    EXPECT_FALSE(stats.executionFailed);
    ASSERT_TRUE(context.results->rayTracedShadowOutput.executionState);
    EXPECT_FALSE(context.results->rayTracedShadowOutput.enabled);
    EXPECT_FALSE(context.results->rayTracedShadowOutput.executionState->dispatchReady.load());
    EXPECT_FALSE(context.results->rayTracedShadowOutput.executionState->executionFailed.load());
}

TEST_F(RenderPassValidationFixture, RayTracedReflectionPassRuntimeCreatesDescriptorSetAndDispatchesRays)
{
    RVX_REQUIRE_RENDER_RUNTIME_PIPELINE(true, true);

    RayTracingSceneManager rayTracingScene;
    ASSERT_NO_FATAL_FAILURE(PrepareRayTracingSceneForSingleObject(device, gpuResources, scene, rayTracingScene));

    RenderGraph graph;
    graph.SetDevice(&device);

    RHITextureDesc colorDesc = RHITextureDesc::RenderTarget(64, 64, RHIFormat::RGBA16_FLOAT);
    colorDesc.debugName = "RayTracedReflectionRuntimeColor";
    RHITextureRef sceneColorTexture = device.CreateTexture(colorDesc);
    ASSERT_TRUE(sceneColorTexture);
    view.colorTarget = graph.ImportTexture(sceneColorTexture.Get(), RHIResourceState::ShaderResource);

    RHITextureDesc depthDesc = RHITextureDesc::DepthStencil(64, 64, PipelineCache::GetDefaultDepthStencilFormat());
    depthDesc.debugName = "RayTracedReflectionRuntimeDepth";
    RHITextureRef depthTexture = device.CreateTexture(depthDesc);
    ASSERT_TRUE(depthTexture);
    view.depthTarget = graph.ImportTexture(depthTexture.Get(), RHIResourceState::DepthRead);

    view.renderGraph = &graph;
    view.viewCache = &viewCache;
    view.viewportWidth = 64;
    view.viewportHeight = 64;
    view.viewCache = &viewCache;
    view.aspectRatio = 1.0f;
    view.fieldOfView = 1.0472f;
    view.nearPlane = 0.1f;
    view.farPlane = 100.0f;
    view.cameraPosition = Vec3(0.0f, 0.0f, 5.0f);
    view.cameraForward = Vec3(0.0f, 0.0f, -1.0f);
    view.viewMatrix = Mat4Identity();
    view.projectionMatrix = Mat4Identity();
    view.viewProjectionMatrix = Mat4Identity();
    view.inverseViewMatrix = Mat4Identity();
    view.inverseProjectionMatrix = Mat4Identity();
    view.previousViewProjectionMatrix = Mat4Identity();
    view.resetTemporalHistory = false;

    RayTracedReflectionPass pass;
    pass.SetEnabled(true);
    pass.OnAdd(&device);
    ConfigureResources(pass, gpuResources, pipelineCache, viewCache);
    pass.SetRayTracingScene(&rayTracingScene);

    ASSERT_TRUE(pass.IsSupported()) << pass.GetUnsupportedReason();

    pass.AddToGraph(graph, view);
    graph.Compile();
    EXPECT_TRUE(graph.GetCompileStats().compileValid);
    EXPECT_EQ(graph.GetCompileStats().totalPasses, 1u);

    RecordingCommandContext ctx;
    graph.Execute(ctx);

    EXPECT_EQ(ctx.dispatchRaysCount, 1u);
    EXPECT_TRUE(ctx.lastDispatchRaysValidation.valid) << ctx.lastDispatchRaysValidation.message;
    EXPECT_EQ(ctx.lastDispatchRaysDesc.width, 64u);
    EXPECT_EQ(ctx.lastDispatchRaysDesc.height, 64u);
    ASSERT_FALSE(ctx.pipelineSequence.empty());
    EXPECT_EQ(ctx.pipelineSequence.back(), pipelineCache.GetRayTracedReflectionPipeline());
    EXPECT_TRUE(std::any_of(ctx.descriptorSetSequence.begin(), ctx.descriptorSetSequence.end(),
                            [](uint32 set) { return set == 0; }));
    ExpectCommandBefore(ctx, "SetPipeline", "DispatchRays");
    ExpectCommandBefore(ctx, "SetDescriptorSet", "DispatchRays");

    const RayTracedReflectionPassStats& stats = pass.GetStats();
    EXPECT_TRUE(stats.outputDeclared);
    EXPECT_TRUE(stats.resourceViewsAvailable);
    EXPECT_TRUE(stats.descriptorSetAvailable);
    EXPECT_TRUE(stats.constantsUploaded);
    EXPECT_TRUE(stats.dispatchRecorded);
    EXPECT_TRUE(stats.historyAvailable);
    EXPECT_EQ(stats.dispatchPixelCount, 64u * 64u);
}

TEST_F(RenderPassValidationFixture, RayTracedReflectionPassRejectsOversizedMaterialTextureTable)
{
    RVX_REQUIRE_RENDER_RUNTIME_PIPELINE(true, true);

    constexpr uint32 kMaterialTextureLimit = RTReflectionBindings::RVX_RT_REFLECTION_MAX_MATERIAL_TEXTURES;
    constexpr uint32 kTextureCount = kMaterialTextureLimit + 1;

    RenderScene oversizedScene;
    std::vector<std::unique_ptr<Resource::MaterialResource>> materials;
    std::vector<Resource::TextureHandle> textures;
    std::vector<uint32_t> visibleObjectIndices;
    materials.reserve(kTextureCount);
    textures.reserve(kTextureCount);
    visibleObjectIndices.reserve(kTextureCount);

    for (uint32 i = 0; i < kTextureCount; ++i)
    {
        Resource::TextureHandle albedo = CreateTextureResource(6000 + i);
        albedo->SetName("RayTracedReflectionOverflowTexture");
        gpuResources.UploadImmediate(albedo.Get());
        ASSERT_TRUE(gpuResources.IsGPUReady(albedo.GetId()));

        auto material = std::make_unique<Resource::MaterialResource>();
        material->SetId(7000 + i);
        material->SetName("RayTracedReflectionOverflowMaterial");
        auto materialData = std::make_shared<Material>("RayTracedReflectionOverflowMaterial");
        TextureInfo baseColorTexture("reflection_overflow_albedo_" + std::to_string(i) + ".png", 0);
        baseColorTexture.offset = Vec2(static_cast<float>(i) * 0.001f, 0.0f);
        materialData->SetBaseColorTexture(baseColorTexture);
        material->SetMaterialData(materialData);
        material->SetTexture("albedo", albedo);
        ASSERT_TRUE(gpuResources.UploadImmediate(material.get()));

        RenderObject object = MakeRenderObject(*meshResource, gpuResources);
        object.entityId = 8000 + i;
        object.material = gpuResources.GetHandle(material->GetId());
        oversizedScene.AddObject(object);
        visibleObjectIndices.push_back(i);

        textures.push_back(albedo);
        materials.push_back(std::move(material));
    }

    RayTracingSceneBuildPlan plan = BuildRayTracingSceneBuildPlan(
        oversizedScene,
        visibleObjectIndices,
        gpuResources.GetRegistry());
    ASSERT_TRUE(plan.HasWork());
    ASSERT_EQ(plan.instances.size(), kTextureCount);

    RayTracingSceneManager rayTracingScene;
    rayTracingScene.Initialize(&device);
    ASSERT_TRUE(rayTracingScene.IsSupported());
    ASSERT_TRUE(rayTracingScene.Prepare(plan)) << rayTracingScene.GetStats().fallbackReason;
    ASSERT_EQ(rayTracingScene.GetInstanceMaterialTextureTable().size(), kTextureCount);
    EXPECT_EQ(rayTracingScene.GetStats().materialTextureCount, kTextureCount);

    RayTracedReflectionPass pass;
    pass.SetEnabled(true);
    pass.OnAdd(&device);
    ConfigureResources(pass, gpuResources, pipelineCache, viewCache);
    pass.SetRayTracingScene(&rayTracingScene);

    EXPECT_FALSE(pass.IsSupported());
    EXPECT_EQ(pass.GetUnsupportedReason(),
              "Ray tracing material texture table exceeds the supported descriptor count");

    RenderGraph graph;
    graph.SetDevice(&device);
    pass.AddToGraph(graph, view);

    const RayTracedReflectionPassStats& stats = pass.GetStats();
    EXPECT_FALSE(stats.supported);
    EXPECT_EQ(stats.materialTextureCount, kMaterialTextureLimit);
    EXPECT_FALSE(stats.materialTextureTableAvailable);
    EXPECT_TRUE(stats.geometryTableAvailable);
    EXPECT_FALSE(stats.outputDeclared);
}

TEST_F(RenderPassValidationFixture, RayTracedReflectionPassRejectsOversizedGeometryBufferTable)
{
    RVX_REQUIRE_RENDER_RUNTIME_PIPELINE(true, true);

    constexpr uint32 kGeometryBufferLimit = RTReflectionBindings::RVX_RT_REFLECTION_MAX_GEOMETRY_BUFFERS;
    constexpr uint32 kMeshCount = kGeometryBufferLimit + 1;

    RenderScene oversizedScene;
    std::vector<std::unique_ptr<Resource::MeshResource>> meshes;
    std::vector<uint32_t> visibleObjectIndices;
    meshes.reserve(kMeshCount);
    visibleObjectIndices.reserve(kMeshCount);

    for (uint32 i = 0; i < kMeshCount; ++i)
    {
        auto mesh = CreateMeshResource(9000 + i);
        gpuResources.UploadImmediate(mesh.get());
        ASSERT_TRUE(gpuResources.IsGPUReady(mesh->GetId()));

        RenderObject object = MakeRenderObject(*mesh, gpuResources);
        object.entityId = 10000 + i;
        oversizedScene.AddObject(object);
        visibleObjectIndices.push_back(i);

        meshes.push_back(std::move(mesh));
    }

    RayTracingSceneBuildPlan plan = BuildRayTracingSceneBuildPlan(
        oversizedScene,
        visibleObjectIndices,
        gpuResources.GetRegistry());
    ASSERT_TRUE(plan.HasWork());
    ASSERT_EQ(plan.instances.size(), kMeshCount);
    ASSERT_EQ(plan.blasBuilds.size(), kMeshCount);

    RayTracingSceneManager rayTracingScene;
    rayTracingScene.Initialize(&device);
    ASSERT_TRUE(rayTracingScene.IsSupported());
    ASSERT_TRUE(rayTracingScene.Prepare(plan)) << rayTracingScene.GetStats().fallbackReason;
    ASSERT_EQ(rayTracingScene.GetInstanceAlphaIndexBufferTable().size(), kMeshCount);
    EXPECT_EQ(rayTracingScene.GetStats().alphaIndexBufferCount, kMeshCount);

    RayTracedReflectionPass pass;
    pass.SetEnabled(true);
    pass.OnAdd(&device);
    ConfigureResources(pass, gpuResources, pipelineCache, viewCache);
    pass.SetRayTracingScene(&rayTracingScene);

    EXPECT_FALSE(pass.IsSupported());
    EXPECT_EQ(pass.GetUnsupportedReason(),
              "Ray tracing geometry buffer table exceeds the supported descriptor count");

    RenderGraph graph;
    graph.SetDevice(&device);
    pass.AddToGraph(graph, view);

    const RayTracedReflectionPassStats& stats = pass.GetStats();
    EXPECT_FALSE(stats.supported);
    EXPECT_TRUE(stats.materialTextureTableAvailable);
    EXPECT_EQ(stats.geometryIndexBufferCount, kGeometryBufferLimit);
    EXPECT_FALSE(stats.geometryTableAvailable);
    EXPECT_FALSE(stats.outputDeclared);
}

TEST_F(RenderPassValidationFixture, RayTracedReflectionRenderGraphChainDenoisesBeforeComposite)
{
    RVX_REQUIRE_RENDER_RUNTIME_PIPELINE(true, true);

    RayTracingSceneManager rayTracingScene;
    ASSERT_NO_FATAL_FAILURE(PrepareRayTracingSceneForSingleObject(device, gpuResources, scene, rayTracingScene));

    RenderGraph graph;
    graph.SetDevice(&device);

    RHITextureDesc colorDesc = RHITextureDesc::RenderTarget(64, 64, RHIFormat::RGBA16_FLOAT);
    colorDesc.debugName = "RayTracedReflectionChainColor";
    RHITextureRef sceneColorTexture = device.CreateTexture(colorDesc);
    ASSERT_TRUE(sceneColorTexture);
    view.colorTarget = graph.ImportTexture(sceneColorTexture.Get(), RHIResourceState::RenderTarget);
    graph.SetExportState(view.colorTarget, RHIResourceState::RenderTarget);

    RHITextureDesc depthDesc = RHITextureDesc::DepthStencil(64, 64, PipelineCache::GetDefaultDepthStencilFormat());
    depthDesc.debugName = "RayTracedReflectionChainDepth";
    RHITextureRef depthTexture = device.CreateTexture(depthDesc);
    ASSERT_TRUE(depthTexture);
    view.depthTarget = graph.ImportTexture(depthTexture.Get(), RHIResourceState::DepthRead);
    graph.SetExportState(view.depthTarget, RHIResourceState::DepthRead);

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
    view.viewMatrix = Mat4Identity();
    view.projectionMatrix = Mat4Identity();
    view.viewProjectionMatrix = Mat4Identity();
    view.inverseViewMatrix = Mat4Identity();
    view.inverseProjectionMatrix = Mat4Identity();
    view.previousViewProjectionMatrix = Mat4Identity();
    view.resetTemporalHistory = false;

    RayTracedReflectionPass reflectionPass;
    reflectionPass.SetEnabled(true);
    reflectionPass.OnAdd(&device);
    ConfigureResources(reflectionPass, gpuResources, pipelineCache, viewCache);
    reflectionPass.SetRayTracingScene(&rayTracingScene);

    RayTracedReflectionDenoisePass denoisePass;
    denoisePass.SetEnabled(true);
    denoisePass.OnAdd(&device);
    denoisePass.SetResources(&pipelineCache, &viewCache);
    denoisePass.SetReflectionSource(&reflectionPass);

    RayTracedReflectionCompositePass compositePass;
    compositePass.SetEnabled(true);
    compositePass.OnAdd(&device);
    compositePass.SetResources(&pipelineCache, &viewCache);
    compositePass.SetReflectionSource(&reflectionPass);
    compositePass.SetDenoisedReflectionSource(&denoisePass);

    EXPECT_LT(reflectionPass.GetPriority(), denoisePass.GetPriority());
    EXPECT_LT(denoisePass.GetPriority(), compositePass.GetPriority());
    ASSERT_TRUE(reflectionPass.IsSupported()) << reflectionPass.GetUnsupportedReason();
    ASSERT_TRUE(denoisePass.IsSupported()) << denoisePass.GetUnsupportedReason();
    ASSERT_TRUE(compositePass.IsSupported()) << compositePass.GetUnsupportedReason();

    reflectionPass.AddToGraph(graph, view);
    denoisePass.AddToGraph(graph, view);
    compositePass.AddToGraph(graph, view);

    const RayTracedReflectionPassStats& reflectionSetupStats = reflectionPass.GetStats();
    const RayTracedReflectionDenoisePassStats& denoiseSetupStats = denoisePass.GetStats();
    const RayTracedReflectionCompositePassStats& compositeSetupStats = compositePass.GetStats();
    EXPECT_TRUE(reflectionSetupStats.outputDeclared);
    EXPECT_TRUE(denoiseSetupStats.outputDeclared);
    EXPECT_TRUE(denoiseSetupStats.reflectionHandleAvailable);
    EXPECT_TRUE(denoiseSetupStats.normalGuideAvailable);
    EXPECT_TRUE(compositeSetupStats.outputDeclared);
    EXPECT_TRUE(compositeSetupStats.denoisedSourceUsed);
    EXPECT_FALSE(compositeSetupStats.rawSourceUsed);
    EXPECT_FALSE(compositeSetupStats.denoiseFallbackToRaw);
    EXPECT_EQ(compositeSetupStats.source, RayTracedReflectionCompositeSource::DenoisedReflection);

    graph.Compile();
    EXPECT_TRUE(graph.GetCompileStats().compileValid);
    EXPECT_EQ(graph.GetCompileStats().totalPasses, 3u);
    EXPECT_EQ(graph.GetCompileStats().culledPasses, 0u);

    RecordingCommandContext ctx;
    graph.Execute(ctx);

    EXPECT_EQ(ctx.dispatchRaysCount, 1u);
    EXPECT_EQ(ctx.drawCount, 2u);
    EXPECT_EQ(ctx.beginRenderPassCount, 2u);
    EXPECT_EQ(ctx.endRenderPassCount, 2u);
    ASSERT_EQ(ctx.pipelineSequence.size(), static_cast<size_t>(3));
    EXPECT_EQ(ctx.pipelineSequence[0], pipelineCache.GetRayTracedReflectionPipeline());
    EXPECT_EQ(ctx.pipelineSequence[1], pipelineCache.GetRayTracedReflectionDenoisePipeline(RHIFormat::RGBA16_FLOAT));
    EXPECT_EQ(ctx.pipelineSequence[2], pipelineCache.GetRayTracedReflectionCompositePipeline(RHIFormat::RGBA16_FLOAT));
    ExpectCommandBefore(ctx, "DispatchRays", "Draw");

    const RayTracedReflectionPassStats& reflectionStats = reflectionPass.GetStats();
    const RayTracedReflectionDenoisePassStats& denoiseStats = denoisePass.GetStats();
    const RayTracedReflectionCompositePassStats& compositeStats = compositePass.GetStats();
    EXPECT_TRUE(reflectionStats.dispatchRecorded);
    EXPECT_TRUE(denoiseStats.denoiseRecorded);
    EXPECT_TRUE(compositeStats.compositeRecorded);
    EXPECT_TRUE(compositeStats.denoisedSourceUsed);
    EXPECT_EQ(compositeStats.source, RayTracedReflectionCompositeSource::DenoisedReflection);
}
TEST_F(RenderPassValidationFixture, RayTracedShadowPassReusesHistoryAcrossStableFrames)
{
    RVX_REQUIRE_RENDER_RUNTIME_PIPELINE(true, true);

    RayTracingSceneManager rayTracingScene;
    ASSERT_NO_FATAL_FAILURE(PrepareRayTracingSceneForSingleObject(device, gpuResources, scene, rayTracingScene));

    RHITextureDesc depthDesc = RHITextureDesc::DepthStencil(64, 64, PipelineCache::GetDefaultDepthStencilFormat());
    depthDesc.debugName = "RayTracedShadowStableHistoryDepth";
    RHITextureRef depthTexture = device.CreateTexture(depthDesc);
    ASSERT_TRUE(depthTexture);

    RayTracedShadowPass pass;
    pass.SetEnabled(true);
    pass.OnAdd(&device);
    ConfigureResources(pass, gpuResources, pipelineCache, viewCache);
    pass.SetRayTracingScene(&rayTracingScene);

    ASSERT_TRUE(pass.IsSupported()) << pass.GetUnsupportedReason();

    auto runFrame = [&](uint64 frameNumber, RecordingCommandContext& ctx)
    {
        RenderGraph graph;
        graph.SetDevice(&device);

        ViewData frameView;
        frameView.renderGraph = &graph;
        frameView.viewCache = &viewCache;
        frameView.depthTarget = graph.ImportTexture(depthTexture.Get(), RHIResourceState::DepthRead);
        frameView.viewportWidth = 64;
        frameView.viewportHeight = 64;
        frameView.aspectRatio = 1.0f;
        frameView.fieldOfView = 1.0472f;
        frameView.nearPlane = 0.1f;
        frameView.farPlane = 100.0f;
        frameView.cameraPosition = Vec3(0.0f, 0.0f, 5.0f);
        frameView.cameraForward = Vec3(0.0f, 0.0f, -1.0f);
        frameView.viewMatrix = Mat4Identity();
        frameView.projectionMatrix = Mat4Identity();
        frameView.viewProjectionMatrix = Mat4Identity();
        frameView.inverseViewMatrix = Mat4Identity();
        frameView.inverseProjectionMatrix = Mat4Identity();
        frameView.previousViewProjectionMatrix = Mat4Identity();
        frameView.frameNumber = frameNumber;
        frameView.resetTemporalHistory = false;

        pass.AddToGraph(graph, frameView);
        graph.Compile();
        EXPECT_TRUE(graph.GetCompileStats().compileValid);
        EXPECT_EQ(graph.GetCompileStats().totalPasses, 1u);

        graph.Execute(ctx);
        pass.NotifySubmission(GPUCompletionToken{});
    };

    RecordingCommandContext firstCtx;
    runFrame(1, firstCtx);
    EXPECT_EQ(firstCtx.dispatchRaysCount, 1u);
    EXPECT_TRUE(firstCtx.lastDispatchRaysValidation.valid) << firstCtx.lastDispatchRaysValidation.message;
    const RayTracedShadowPassStats firstStats = pass.GetStats();
    EXPECT_TRUE(firstStats.dispatchRecorded);
    EXPECT_TRUE(firstStats.historyRecreated);
    EXPECT_FALSE(firstStats.historyAvailable);
    EXPECT_FALSE(firstStats.temporalAccumulated);
    const size_t textureCountAfterFirstFrame = device.createdTextureDescs.size();

    RecordingCommandContext secondCtx;
    runFrame(2, secondCtx);
    EXPECT_EQ(secondCtx.dispatchRaysCount, 1u);
    EXPECT_TRUE(secondCtx.lastDispatchRaysValidation.valid) << secondCtx.lastDispatchRaysValidation.message;
    const RayTracedShadowPassStats secondStats = pass.GetStats();
    EXPECT_TRUE(secondStats.dispatchRecorded);
    EXPECT_TRUE(secondStats.historyAvailable);
    EXPECT_TRUE(secondStats.depthHistoryAvailable);
    EXPECT_TRUE(secondStats.normalHistoryAvailable);
    EXPECT_TRUE(secondStats.temporalAccumulated);
    EXPECT_FALSE(secondStats.historyRecreated);
    EXPECT_FALSE(secondStats.historyResolutionChanged);
    EXPECT_FALSE(secondStats.historyConfigChanged);
    EXPECT_FALSE(secondStats.historyReset);
    // Velocity is optional at the view boundary, but the RT descriptor always
    // needs a graph-declared SRV.  Each recording therefore owns one tiny
    // fallback instead of sharing a cross-graph Common-state texture.
    EXPECT_EQ(device.createdTextureDescs.size(), textureCountAfterFirstFrame + 1u);
    ASSERT_NE(device.createdTextureDescs.back().debugName, nullptr);
    EXPECT_EQ(std::string(device.createdTextureDescs.back().debugName),
              "RayTracedShadowFallbackVelocity");
}

TEST_F(RenderPassValidationFixture,
       RayTracedShadowPassTypedHistoryResetAndResizeWriteFramesDoNotReadOldInputs)
{
    RVX_REQUIRE_RENDER_RUNTIME_PIPELINE(true, true);

    RayTracingSceneManager rayTracingScene;
    ASSERT_NO_FATAL_FAILURE(PrepareRayTracingSceneForSingleObject(
        device, gpuResources, scene, rayTracingScene));

    RayTracedShadowPass pass;
    pass.SetEnabled(true);
    pass.OnAdd(&device);
    ConfigureResources(pass, gpuResources, pipelineCache, viewCache);
    pass.SetRayTracingScene(&rayTracingScene);
    ASSERT_TRUE(pass.IsSupported()) << pass.GetUnsupportedReason();

    const auto runFrame = [&](uint64 frameNumber,
                              uint32 extent,
                              bool resetTemporalHistory) -> RayTracedShadowPassStats
    {
        RenderGraph graph;
        graph.SetDevice(&device);

        RHITextureDesc depthDesc = RHITextureDesc::DepthStencil(
            extent, extent, PipelineCache::GetDefaultDepthStencilFormat());
        depthDesc.debugName = "RayTracedShadowTypedHistoryDepth";
        RHITextureRef depthTexture = device.CreateTexture(depthDesc);
        EXPECT_TRUE(depthTexture);
        if (!depthTexture)
        {
            return {};
        }

        ViewData frameView;
        frameView.renderGraph = &graph;
        frameView.viewCache = &viewCache;
        frameView.depthTarget = graph.ImportTexture(
            depthTexture.Get(), RHIResourceState::DepthRead);
        frameView.viewportWidth = extent;
        frameView.viewportHeight = extent;
        frameView.aspectRatio = 1.0f;
        frameView.fieldOfView = 1.0472f;
        frameView.nearPlane = 0.1f;
        frameView.farPlane = 100.0f;
        frameView.cameraPosition = Vec3(0.0f, 0.0f, 5.0f);
        frameView.cameraForward = Vec3(0.0f, 0.0f, -1.0f);
        frameView.viewMatrix = Mat4Identity();
        frameView.projectionMatrix = Mat4Identity();
        frameView.viewProjectionMatrix = Mat4Identity();
        frameView.inverseViewMatrix = Mat4Identity();
        frameView.inverseProjectionMatrix = Mat4Identity();
        frameView.previousViewProjectionMatrix = Mat4Identity();
        frameView.frameNumber = frameNumber;
        frameView.resetTemporalHistory = resetTemporalHistory;

        RenderPassRecordContext context = MakeRenderPassRecordContext(graph, frameView);
        context.legacyAdapter = false;
        context.results = std::make_shared<RenderPassRecordResults>();
        ConfigurePrimaryDirectionalLightRecord(
            context, Vec3{-0.3f, -1.0f, -0.2f},
            Vec3{1.0f, 1.0f, 1.0f}, 1.0f);
        EXPECT_TRUE(context.frameSnapshot);
        if (!context.frameSnapshot)
        {
            return {};
        }

        pass.AddToGraph(graph, context);
        graph.Compile();
        EXPECT_TRUE(graph.GetCompileStats().compileValid);
        EXPECT_EQ(graph.GetCompileStats().totalPasses, 1u);

        RecordingCommandContext commands;
        graph.Execute(commands);
        EXPECT_EQ(commands.dispatchRaysCount, 1u);
        EXPECT_TRUE(commands.lastDispatchRaysValidation.valid)
            << commands.lastDispatchRaysValidation.message;
        const RayTracedShadowPassStats stats =
            context.results->rayTracedShadowStats;
        pass.NotifySubmission(context.identity, GPUCompletionToken{});
        return stats;
    };

    const RayTracedShadowPassStats initialStats = runFrame(1, 64, false);
    EXPECT_TRUE(initialStats.dispatchRecorded);
    EXPECT_TRUE(initialStats.historyRecreated);
    EXPECT_FALSE(initialStats.historyAvailable);
    EXPECT_FALSE(initialStats.depthHistoryAvailable);
    EXPECT_FALSE(initialStats.normalHistoryAvailable);
    EXPECT_FALSE(initialStats.temporalAccumulated);

    const RayTracedShadowPassStats stableStats = runFrame(2, 64, false);
    EXPECT_TRUE(stableStats.dispatchRecorded);
    EXPECT_TRUE(stableStats.historyAvailable);
    EXPECT_TRUE(stableStats.depthHistoryAvailable);
    EXPECT_TRUE(stableStats.normalHistoryAvailable);
    EXPECT_TRUE(stableStats.temporalAccumulated);

    const RayTracedShadowPassStats resetWriteStats = runFrame(3, 64, true);
    EXPECT_TRUE(resetWriteStats.requested);
    EXPECT_TRUE(resetWriteStats.supported);
    EXPECT_TRUE(resetWriteStats.dispatchRecorded);
    EXPECT_TRUE(resetWriteStats.historyReset);
    EXPECT_FALSE(resetWriteStats.historyAvailable);
    EXPECT_FALSE(resetWriteStats.depthHistoryAvailable);
    EXPECT_FALSE(resetWriteStats.normalHistoryAvailable);
    EXPECT_FALSE(resetWriteStats.temporalAccumulated);

    const RayTracedShadowPassStats resetRecoveryStats = runFrame(4, 64, false);
    EXPECT_TRUE(resetRecoveryStats.historyAvailable);
    EXPECT_TRUE(resetRecoveryStats.depthHistoryAvailable);
    EXPECT_TRUE(resetRecoveryStats.normalHistoryAvailable);
    EXPECT_TRUE(resetRecoveryStats.temporalAccumulated);

    const RayTracedShadowPassStats resizeWriteStats = runFrame(5, 96, false);
    EXPECT_TRUE(resizeWriteStats.requested);
    EXPECT_TRUE(resizeWriteStats.supported);
    EXPECT_TRUE(resizeWriteStats.dispatchRecorded);
    EXPECT_TRUE(resizeWriteStats.historyReset);
    EXPECT_TRUE(resizeWriteStats.historyRecreated);
    EXPECT_TRUE(resizeWriteStats.historyResolutionChanged);
    EXPECT_EQ(resizeWriteStats.width, 96u);
    EXPECT_EQ(resizeWriteStats.height, 96u);
    EXPECT_FALSE(resizeWriteStats.historyAvailable);
    EXPECT_FALSE(resizeWriteStats.depthHistoryAvailable);
    EXPECT_FALSE(resizeWriteStats.normalHistoryAvailable);
    EXPECT_FALSE(resizeWriteStats.temporalAccumulated);

    const RayTracedShadowPassStats resizeRecoveryStats = runFrame(6, 96, false);
    EXPECT_TRUE(resizeRecoveryStats.historyAvailable);
    EXPECT_TRUE(resizeRecoveryStats.depthHistoryAvailable);
    EXPECT_TRUE(resizeRecoveryStats.normalHistoryAvailable);
    EXPECT_TRUE(resizeRecoveryStats.temporalAccumulated);
}

TEST_F(RenderPassValidationFixture, RayTracedShadowPassScopesSubmissionAndReleaseByRecordIdentity)
{
    RVX_REQUIRE_RENDER_RUNTIME_PIPELINE(true, true);
    device.EnableTimestampQueries();

    RayTracingSceneManager rayTracingScene;
    ASSERT_NO_FATAL_FAILURE(PrepareRayTracingSceneForSingleObject(
        device, gpuResources, scene, rayTracingScene));

    RayTracedShadowPass pass;
    pass.SetEnabled(true);
    pass.OnAdd(&device);
    ConfigureResources(pass, gpuResources, pipelineCache, viewCache);
    pass.SetRayTracingScene(&rayTracingScene);
    ASSERT_TRUE(pass.IsSupported()) << pass.GetUnsupportedReason();

    std::vector<RHITextureRef> recordDepthTextures;
    const auto makeRecord = [&](RenderGraph& graph, uint64 frameNumber)
    {
        RHITextureRef depthTexture = device.CreateTexture(RHITextureDesc::DepthStencil(
            64, 64, PipelineCache::GetDefaultDepthStencilFormat()));
        EXPECT_TRUE(depthTexture);
        recordDepthTextures.push_back(depthTexture);

        ViewData frameView;
        frameView.renderGraph = &graph;
        frameView.viewCache = &viewCache;
        frameView.depthTarget = graph.ImportTexture(depthTexture.Get(), RHIResourceState::DepthRead);
        frameView.viewportWidth = 64;
        frameView.viewportHeight = 64;
        frameView.frameNumber = frameNumber;
        frameView.nearPlane = 0.1f;
        frameView.farPlane = 100.0f;
        frameView.viewMatrix = Mat4Identity();
        frameView.projectionMatrix = Mat4Identity();
        frameView.viewProjectionMatrix = Mat4Identity();
        frameView.inverseViewMatrix = Mat4Identity();
        frameView.inverseProjectionMatrix = Mat4Identity();

        RenderPassRecordContext context = MakeRenderPassRecordContext(graph, frameView);
        // This helper models renderer-issued identity-aware recording.  The
        // ViewData overload below deliberately exercises the legacy adapter.
        context.legacyAdapter = false;
        context.results = std::make_shared<RenderPassRecordResults>();
        context.frameSnapshot = MakeRenderPassFrameSnapshot(context, *context.results);
        return context;
    };
    const auto countConstants = [this]()
    {
        return static_cast<uint32>(std::count_if(
            device.createdBufferDescs.begin(), device.createdBufferDescs.end(),
            [](const RHIBufferDesc& desc)
            {
                return desc.debugName != nullptr &&
                       std::string(desc.debugName) == "RayTracedShadowConstants";
            }));
    };
    const auto countBuffersNamed = [this](const char* debugName)
    {
        return static_cast<uint32>(std::count_if(
            device.createdBufferDescs.begin(), device.createdBufferDescs.end(),
            [debugName](const RHIBufferDesc& desc)
            {
                return desc.debugName != nullptr && std::string(desc.debugName) == debugName;
            }));
    };
    const auto findTextureBinding = [](const RHIDescriptorSetDesc& desc, uint32 binding)
    {
        const auto it = std::find_if(desc.bindings.begin(), desc.bindings.end(),
            [binding](const RHIDescriptorBinding& candidate)
            {
                return candidate.binding == binding;
            });
        return it != desc.bindings.end() && it->textureView
            ? it->textureView->GetTexture() : nullptr;
    };

    ShadowPassConfig configA = pass.GetConfig();
    configA.rayTracedSamplesPerPixel = 1;
    ShadowPassConfig configB = configA;
    configB.rayTracedSamplesPerPixel = 2;

    RenderGraph graphA;
    RenderGraph graphB;
    graphA.SetDevice(&device);
    graphB.SetDevice(&device);
    RenderPassRecordContext contextA = makeRecord(graphA, 100);
    RenderPassRecordContext contextB = makeRecord(graphB, 101);
    ConfigurePrimaryDirectionalLightRecord(
        contextA, Vec3{1.0f, 0.0f, 0.0f}, Vec3{1.0f, 0.4f, 0.2f}, 2.0f);
    ConfigurePrimaryDirectionalLightRecord(
        contextB, Vec3{0.0f, 1.0f, 0.0f}, Vec3{0.2f, 0.7f, 1.0f}, 3.0f);
    ASSERT_TRUE(contextA.frameSnapshot);
    ASSERT_TRUE(contextB.frameSnapshot);
    EXPECT_FLOAT_EQ(contextA.frameSnapshot->primaryDirectionalLight.direction.x, 1.0f);
    EXPECT_FLOAT_EQ(contextB.frameSnapshot->primaryDirectionalLight.direction.y, 1.0f);
    const uint32 constantsBefore = countConstants();
    const uint32 timingReadbacksBefore = countBuffersNamed("RayTracedShadowTimingReadback");
    const uint32 timingPoolsBefore = static_cast<uint32>(device.createdQueryPoolDescs.size());
    pass.SetConfig(configA);
    pass.AddToGraph(graphA, contextA);
    pass.SetConfig(configB);
    pass.AddToGraph(graphB, contextB);
    EXPECT_EQ(constantsBefore + 2, countConstants());
    EXPECT_EQ(timingReadbacksBefore + 2, countBuffersNamed("RayTracedShadowTimingReadback"));
    EXPECT_EQ(timingPoolsBefore + 2, device.createdQueryPoolDescs.size());
    EXPECT_EQ(1u, contextA.results->rayTracedShadowStats.gpuTimingReadbackBufferCount);
    EXPECT_EQ(sizeof(uint64) * 2u,
              contextA.results->rayTracedShadowStats.gpuTimingReadbackBytes);
    EXPECT_EQ(RVX_INVALID_INDEX,
              contextA.results->rayTracedShadowStats.gpuTimingReadbackFrameIndex);
    EXPECT_EQ(1u, contextB.results->rayTracedShadowStats.gpuTimingReadbackBufferCount);
    EXPECT_EQ(sizeof(uint64) * 2u,
              contextB.results->rayTracedShadowStats.gpuTimingReadbackBytes);
    EXPECT_EQ(RVX_INVALID_INDEX,
              contextB.results->rayTracedShadowStats.gpuTimingReadbackFrameIndex);
    ASSERT_TRUE(contextA.results->rayTracedShadowOutput.executionState);
    ASSERT_TRUE(contextB.results->rayTracedShadowOutput.executionState);
    EXPECT_NE(contextA.results->rayTracedShadowOutput.executionState,
              contextB.results->rayTracedShadowOutput.executionState);
    EXPECT_NE(contextA.results->rayTracedShadowOutput.identity,
              contextB.results->rayTracedShadowOutput.identity);
    ASSERT_TRUE(contextA.results->rayTracedShadowOutput.shadowMask.IsValid());
    ASSERT_TRUE(contextB.results->rayTracedShadowOutput.shadowMask.IsValid());

    const auto collectRayShadowConstantBuffers = [this]()
    {
        std::vector<const FakeBuffer*> buffers;
        for (size_t index = 0;
             index < device.createdBufferDescs.size() &&
             index < device.createdBuffers.size();
             ++index)
        {
            const char* debugName = device.createdBufferDescs[index].debugName;
            if (debugName != nullptr &&
                std::string(debugName) == "RayTracedShadowConstants")
            {
                buffers.push_back(device.createdBuffers[index]);
            }
        }
        return buffers;
    };
    const std::vector<const FakeBuffer*> rayConstantBuffers =
        collectRayShadowConstantBuffers();
    ASSERT_EQ(static_cast<size_t>(constantsBefore + 2),
              rayConstantBuffers.size());
    const FakeBuffer* const constantsA = rayConstantBuffers[rayConstantBuffers.size() - 2];
    const FakeBuffer* const constantsB = rayConstantBuffers.back();
    ASSERT_NE(constantsA, nullptr);
    ASSERT_NE(constantsB, nullptr);
    graphA.Compile();
    graphB.Compile();
    ASSERT_TRUE(graphA.GetCompileStats().compileValid);
    ASSERT_TRUE(graphB.GetCompileStats().compileValid);
    RHITexture* const outputA = graphA.GetTexture(contextA.results->rayTracedShadowOutput.shadowMask);
    RHITexture* const outputB = graphB.GetTexture(contextB.results->rayTracedShadowOutput.shadowMask);
    ASSERT_NE(outputA, nullptr);
    ASSERT_NE(outputB, nullptr);
    EXPECT_NE(outputA, outputB);

    // Execute B before A, then submit B first. A's older identity must never
    // overwrite B when it is submitted later.
    const size_t descriptorCountBefore = device.createdDescriptorSetDescs.size();
    RecordingCommandContext commandsB;
    RecordingCommandContext commandsA;
    graphB.Execute(commandsB);
    graphA.Execute(commandsA);
    ASSERT_EQ(1u, commandsA.dispatchRaysCount);
    ASSERT_EQ(1u, commandsB.dispatchRaysCount);
    ASSERT_GE(constantsA->GetStorage().size(), sizeof(Mat4) * 2 + sizeof(Vec4));
    ASSERT_GE(constantsB->GetStorage().size(), sizeof(Mat4) * 2 + sizeof(Vec4));
    Vec4 recordedRayA{};
    Vec4 recordedRayB{};
    std::memcpy(&recordedRayA,
                constantsA->GetStorage().data() + sizeof(Mat4) * 2,
                sizeof(recordedRayA));
    std::memcpy(&recordedRayB,
                constantsB->GetStorage().data() + sizeof(Mat4) * 2,
                sizeof(recordedRayB));
    EXPECT_FLOAT_EQ(recordedRayA.x, -1.0f);
    EXPECT_FLOAT_EQ(recordedRayA.y, 0.0f);
    EXPECT_FLOAT_EQ(recordedRayA.z, 0.0f);
    EXPECT_FLOAT_EQ(recordedRayB.x, 0.0f);
    EXPECT_FLOAT_EQ(recordedRayB.y, -1.0f);
    EXPECT_FLOAT_EQ(recordedRayB.z, 0.0f);
    ASSERT_EQ(descriptorCountBefore + 2, device.createdDescriptorSetDescs.size());
    const RHIDescriptorSetDesc& descriptorB = device.createdDescriptorSetDescs[descriptorCountBefore];
    const RHIDescriptorSetDesc& descriptorA = device.createdDescriptorSetDescs[descriptorCountBefore + 1];
    EXPECT_EQ(findTextureBinding(descriptorA, RTShadowBindings::RVX_RT_SHADOW_OUTPUT_MASK_BINDING), outputA);
    EXPECT_EQ(findTextureBinding(descriptorB, RTShadowBindings::RVX_RT_SHADOW_OUTPUT_MASK_BINDING), outputB);
    EXPECT_NE(findTextureBinding(descriptorA, RTShadowBindings::RVX_RT_SHADOW_PREVIOUS_MASK_BINDING),
              findTextureBinding(descriptorB, RTShadowBindings::RVX_RT_SHADOW_PREVIOUS_MASK_BINDING));
    EXPECT_NE(findTextureBinding(descriptorA, RTShadowBindings::RVX_RT_SHADOW_PREVIOUS_DEPTH_BINDING),
              findTextureBinding(descriptorB, RTShadowBindings::RVX_RT_SHADOW_PREVIOUS_DEPTH_BINDING));
    EXPECT_NE(findTextureBinding(descriptorA, RTShadowBindings::RVX_RT_SHADOW_PREVIOUS_NORMAL_BINDING),
              findTextureBinding(descriptorB, RTShadowBindings::RVX_RT_SHADOW_PREVIOUS_NORMAL_BINDING));
    EXPECT_NE(findTextureBinding(descriptorA, RTShadowBindings::RVX_RT_SHADOW_SCENE_VELOCITY_BINDING),
              findTextureBinding(descriptorB, RTShadowBindings::RVX_RT_SHADOW_SCENE_VELOCITY_BINDING));
    EXPECT_NE(findTextureBinding(descriptorA, RTShadowBindings::RVX_RT_SHADOW_PREVIOUS_MASK_BINDING), outputA);
    EXPECT_NE(findTextureBinding(descriptorB, RTShadowBindings::RVX_RT_SHADOW_PREVIOUS_MASK_BINDING), outputB);
    EXPECT_TRUE(contextA.results->rayTracedShadowOutput.executionState->dispatchReady.load());
    EXPECT_TRUE(contextB.results->rayTracedShadowOutput.executionState->dispatchReady.load());
    EXPECT_EQ(graphA.GetRealizedAccess(
                  contextA.results->rayTracedShadowOutput.shadowMask).uniformAccess.layout,
              RHIResourceLayout::ShaderReadOnly);
    EXPECT_EQ(graphB.GetRealizedAccess(
                  contextB.results->rayTracedShadowOutput.shadowMask).uniformAccess.layout,
              RHIResourceLayout::ShaderReadOnly);
    pass.NotifySubmission(contextB.identity, GPUCompletionToken{});
    pass.NotifySubmission(contextA.identity, GPUCompletionToken{});
    EXPECT_EQ(pass.GetStats().samplesPerPixel, configB.rayTracedSamplesPerPixel);

    // Submitting C must not erase D's reservation.  This exercises the
    // identity-scoped notify path independently of inverse-order submission.
    RenderGraph graphC;
    RenderGraph graphD;
    graphC.SetDevice(&device);
    graphD.SetDevice(&device);
    RenderPassRecordContext contextC = makeRecord(graphC, 102);
    RenderPassRecordContext contextD = makeRecord(graphD, 103);
    pass.SetConfig(configA);
    pass.AddToGraph(graphC, contextC);
    pass.SetConfig(configB);
    pass.AddToGraph(graphD, contextD);
    graphC.Compile();
    graphD.Compile();
    ASSERT_TRUE(graphC.GetCompileStats().compileValid);
    ASSERT_TRUE(graphD.GetCompileStats().compileValid);
    RecordingCommandContext commandsC;
    RecordingCommandContext commandsD;
    graphC.Execute(commandsC);
    graphD.Execute(commandsD);
    pass.NotifySubmission(contextC.identity, GPUCompletionToken{});
    EXPECT_EQ(pass.GetStats().samplesPerPixel, configA.rayTracedSamplesPerPixel);
    pass.NotifySubmission(contextD.identity, GPUCompletionToken{});
    EXPECT_EQ(pass.GetStats().samplesPerPixel, configB.rayTracedSamplesPerPixel);

    // Releasing E must not discard F.  F uses the still-committed B/D
    // history and submits normally after E is abandoned.
    RenderGraph abandonedGraph;
    RenderGraph retainedGraph;
    abandonedGraph.SetDevice(&device);
    retainedGraph.SetDevice(&device);
    RenderPassRecordContext abandoned = makeRecord(abandonedGraph, 104);
    RenderPassRecordContext retained = makeRecord(retainedGraph, 105);
    pass.SetConfig(configA);
    pass.AddToGraph(abandonedGraph, abandoned);
    pass.SetConfig(configB);
    pass.AddToGraph(retainedGraph, retained);
    ASSERT_TRUE(retained.results->rayTracedShadowStats.historyAvailable);
    pass.ReleaseUnsubmittedFrame(abandoned.identity);
    retainedGraph.Compile();
    ASSERT_TRUE(retainedGraph.GetCompileStats().compileValid);
    RecordingCommandContext retainedCommands;
    retainedGraph.Execute(retainedCommands);
    ASSERT_EQ(1u, retainedCommands.dispatchRaysCount);
    pass.NotifySubmission(retained.identity, GPUCompletionToken{});
    EXPECT_EQ(pass.GetStats().samplesPerPixel, configB.rayTracedSamplesPerPixel);

    RenderGraph nextGraph;
    nextGraph.SetDevice(&device);
    RenderPassRecordContext next = makeRecord(nextGraph, 106);
    pass.SetConfig(configB);
    pass.AddToGraph(nextGraph, next);
    EXPECT_TRUE(next.results->rayTracedShadowStats.historyAvailable);
    EXPECT_FALSE(next.results->rayTracedShadowStats.historyConfigChanged);
    pass.ReleaseUnsubmittedFrame(next.identity);

    // A newer submitted recording that never reaches graph execution still
    // owns the submission watermark.  A delayed older success must neither
    // overwrite its diagnostics nor roll temporal history back to config A.
    RenderGraph delayedGraph;
    RenderGraph rejectedGraph;
    delayedGraph.SetDevice(&device);
    rejectedGraph.SetDevice(&device);
    RenderPassRecordContext delayed = makeRecord(delayedGraph, 108);
    RenderPassRecordContext rejected = makeRecord(rejectedGraph, 109);
    pass.SetConfig(configA);
    pass.AddToGraph(delayedGraph, delayed);
    pass.SetConfig(configB);
    pass.AddToGraph(rejectedGraph, rejected);
    delayedGraph.Compile();
    ASSERT_TRUE(delayedGraph.GetCompileStats().compileValid);
    RecordingCommandContext delayedCommands;
    delayedGraph.Execute(delayedCommands);
    ASSERT_EQ(1u, delayedCommands.dispatchRaysCount);
    pass.NotifySubmission(rejected.identity, GPUCompletionToken{});
    EXPECT_EQ(pass.GetStats().samplesPerPixel, configB.rayTracedSamplesPerPixel);
    EXPECT_FALSE(pass.GetStats().dispatchRecorded);
    pass.NotifySubmission(delayed.identity, GPUCompletionToken{});
    EXPECT_EQ(pass.GetStats().samplesPerPixel, configB.rayTracedSamplesPerPixel);
    EXPECT_FALSE(pass.GetStats().dispatchRecorded);

    RenderGraph successorGraph;
    successorGraph.SetDevice(&device);
    RenderPassRecordContext successor = makeRecord(successorGraph, 110);
    pass.SetConfig(configB);
    pass.AddToGraph(successorGraph, successor);
    EXPECT_TRUE(successor.results->rayTracedShadowStats.historyAvailable);
    EXPECT_FALSE(successor.results->rayTracedShadowStats.historyConfigChanged);
    pass.ReleaseUnsubmittedFrame(successor.identity);

    // Compatibility-only backends report a satisfied completion as
    // CompatibilityWaitIdle after FrameSynchronizer has waited.  The shadow
    // timing readback must be consumed just like a native completed fence.
    FakeDevice compatibilityDevice;
    compatibilityDevice.EnableCompatibilityWaitIdleCompletion();
    RenderSubmissionTracker compatibilityTracker;
    ASSERT_TRUE(compatibilityTracker.Initialize(&compatibilityDevice));
    pass.SetSubmissionTracker(&compatibilityTracker);

    RenderGraph timingGraph;
    timingGraph.SetDevice(&device);
    RenderPassRecordContext timing = makeRecord(timingGraph, 111);
    pass.SetConfig(configB);
    pass.AddToGraph(timingGraph, timing);
    timingGraph.Compile();
    ASSERT_TRUE(timingGraph.GetCompileStats().compileValid);
    RecordingCommandContext timingCommands;
    timingGraph.Execute(timingCommands);
    ASSERT_EQ(1u, timingCommands.dispatchRaysCount);
    GPUCompletionToken timingCompletion;
    ASSERT_TRUE(InsertGPUCompletionPoint(
        timingCompletion, compatibilityTracker.Submit(&timingCommands)));
    pass.NotifySubmission(timing.identity, timingCompletion);
    EXPECT_FALSE(pass.GetStats().gpuTimingResultAvailable);
    EXPECT_EQ(compatibilityTracker.Wait(timingCompletion),
              GPUCompletionStatus::CompatibilityWaitIdle);

    // The policy path must poll even if the pass is disabled and is therefore
    // absent from the graph registry for this frame.
    pass.SetEnabled(false);
    pass.RefreshCompletionDiagnostics();
    EXPECT_TRUE(pass.GetStats().gpuTimingResultAvailable);
    EXPECT_EQ(pass.GetStats().gpuTimingElapsedTicks, 0u);
    pass.SetSubmissionTracker(nullptr);
    compatibilityTracker.Shutdown();

    // The first legacy recording rejects before reserving history.  Its
    // pending legacy record must still block a second legacy Add, and no-id
    // Notify must publish/release that first rejection rather than silently
    // completing the second graph.
    pass.SetEnabled(true);
    RenderGraph legacyRejectedGraph;
    legacyRejectedGraph.SetDevice(&device);
    ViewData legacyRejectedView;
    legacyRejectedView.renderGraph = &legacyRejectedGraph;
    legacyRejectedView.frameNumber = 112;
    legacyRejectedView.viewportWidth = 64;
    legacyRejectedView.viewportHeight = 64;
    pass.AddToGraph(legacyRejectedGraph, legacyRejectedView);

    RenderGraph blockedLegacyGraph;
    blockedLegacyGraph.SetDevice(&device);
    RenderPassRecordContext blockedLegacyRecord = makeRecord(blockedLegacyGraph, 113);
    pass.AddToGraph(blockedLegacyGraph, blockedLegacyRecord.view);
    blockedLegacyGraph.Compile();
    ASSERT_TRUE(blockedLegacyGraph.GetCompileStats().compileValid);
    RecordingCommandContext blockedLegacyCommands;
    blockedLegacyGraph.Execute(blockedLegacyCommands);
    EXPECT_EQ(0u, blockedLegacyCommands.dispatchRaysCount);
    pass.NotifySubmission(GPUCompletionToken{});
    EXPECT_EQ(0u, pass.GetStats().width);
    EXPECT_FALSE(pass.GetStats().dispatchRecorded);

    RenderGraph completedLegacyGraph;
    completedLegacyGraph.SetDevice(&device);
    RenderPassRecordContext completedLegacyRecord = makeRecord(completedLegacyGraph, 114);
    pass.AddToGraph(completedLegacyGraph, completedLegacyRecord.view);
    completedLegacyGraph.Compile();
    ASSERT_TRUE(completedLegacyGraph.GetCompileStats().compileValid);
    RecordingCommandContext completedLegacyCommands;
    completedLegacyGraph.Execute(completedLegacyCommands);
    ASSERT_EQ(1u, completedLegacyCommands.dispatchRaysCount);
    pass.NotifySubmission(GPUCompletionToken{});
    EXPECT_TRUE(pass.GetStats().dispatchRecorded);

    RenderGraph releasedLegacyGraph;
    releasedLegacyGraph.SetDevice(&device);
    RenderPassRecordContext releasedLegacyRecord = makeRecord(releasedLegacyGraph, 115);
    pass.AddToGraph(releasedLegacyGraph, releasedLegacyRecord.view);
    pass.ReleaseUnsubmittedFrame();

    RenderGraph postReleaseLegacyGraph;
    postReleaseLegacyGraph.SetDevice(&device);
    RenderPassRecordContext postReleaseLegacyRecord = makeRecord(postReleaseLegacyGraph, 116);
    pass.AddToGraph(postReleaseLegacyGraph, postReleaseLegacyRecord.view);
    postReleaseLegacyGraph.Compile();
    ASSERT_TRUE(postReleaseLegacyGraph.GetCompileStats().compileValid);
    RecordingCommandContext postReleaseLegacyCommands;
    postReleaseLegacyGraph.Execute(postReleaseLegacyCommands);
    ASSERT_EQ(1u, postReleaseLegacyCommands.dispatchRaysCount);
    pass.NotifySubmission(GPUCompletionToken{});
}

TEST_F(RenderPassValidationFixture,
       OpaquePassRetainsFrameBindingsPerSubmissionAcrossOutOfOrderGraphs)
{
    RVX_REQUIRE_RENDER_RUNTIME_PIPELINE();
    device.SetFenceAutoComplete(false);

    RenderSubmissionTracker tracker;
    ASSERT_TRUE(tracker.Initialize(&device));
    RenderRetirementQueue retirement;
    ASSERT_TRUE(retirement.Initialize(&tracker));

    RenderDrawItem opaqueItem = MakeDrawItem(MaterialRenderMode::Opaque);
    std::vector<RenderDrawItem> opaqueItems = {opaqueItem};
    std::vector<RenderDrawItem> maskedItems;
    const MeshGPUBuffers buffers = gpuResources.GetMeshBuffers(meshResource->GetId());
    ASSERT_TRUE(buffers.IsValid());
    ASSERT_FALSE(buffers.submeshes.empty());
    opaqueItems.front().packet = MakeOpaquePacket(
        scene, 0, 0, buffers, opaqueItems.front().material,
        RenderMaterialMode::Opaque, RenderDrawFlags::None);
    SceneMeshPassPreparation preparation = PrepareOpaquePackets(
        opaqueItems, maskedItems);

    GPUCulling culling;
    GPUCullingConfig cullingConfig;
    cullingConfig.maxInstances = 2;
    cullingConfig.enableOcclusionCulling = false;
    cullingConfig.enableDistanceCulling = false;
    culling.Initialize(&device, cullingConfig);
    culling.BeginFrame();
    ASSERT_EQ(0u, culling.BeginDrawGroup(
        meshResource->GetId(), opaqueItem.material.slot,
        MaterialPipelineVariant::Opaque, opaqueItem.mesh, opaqueItem.material));
    GPUIndexedDrawDesc drawDesc;
    drawDesc.indexCount = buffers.submeshes[0].indexCount;
    drawDesc.firstIndex = buffers.submeshes[0].indexOffset;
    drawDesc.vertexOffset = buffers.submeshes[0].baseVertex;
    EXPECT_NE(RVX_INVALID_INDEX,
              culling.AddDrawItemInstance(scene, opaqueItem, drawDesc, 0));
    culling.EndDrawGroup();
    culling.EndFrame();
    culling.CullCpuFallback(view.viewMatrix, view.projectionMatrix);

    RenderSubmissionResourceBatch batchA;
    RenderSubmissionResourceBatch batchB;
    std::vector<RHITextureRef> retainedMasks;
    const auto recordOpaque = [&](RenderGraph& graph,
                                  OpaquePass& pass,
                                  RenderSubmissionResourceBatch& batch,
                                  const char* colorDebugName,
                                  const char* maskDebugName,
                                  uint64 frameSequence)
    {
        ViewData graphView = view;
        graphView.renderGraph = &graph;
        graphView.viewCache = &viewCache;
        graphView.submissionResourceBatch = &batch;
        RHITextureDesc colorDesc = RHITextureDesc::RenderTarget(
            64, 64, RHIFormat::RGBA8_UNORM);
        colorDesc.debugName = colorDebugName;
        graphView.colorTarget = graph.CreateTexture(colorDesc);
        graph.SetExportState(graphView.colorTarget, RHIResourceState::RenderTarget);

        RHITextureDesc maskDesc = RHITextureDesc::Texture2D(
            64, 64, RHIFormat::R8_UNORM, RHITextureUsage::ShaderResource);
        maskDesc.debugName = maskDebugName;
        RHITextureRef mask = device.CreateTexture(maskDesc);
        ASSERT_TRUE(mask);
        retainedMasks.push_back(mask);

        auto executionState = std::make_shared<RayTracedShadowExecutionState>();
        executionState->dispatchReady.store(true, std::memory_order_release);
        RayTracedShadowRecordOutput rayTracedInputs;
        rayTracedInputs.enabled = true;
        rayTracedInputs.executionState = executionState;
        rayTracedInputs.shadowMask = graph.ImportTexture(
            mask.Get(), RHIResourceState::ShaderResource);

        const RenderFramePlanCompileResult compiled = CompileForcedGPUPlan(
            preparation, frameSequence);
        ASSERT_TRUE(compiled.succeeded);
        RenderFrameExecutionReport report = MakeExecutionReport(compiled.plan);
        ConfigureResources(pass, gpuResources, pipelineCache, materialSystem);
        RenderPassRecordContext context = MakeMainSceneRecordContext(
            graph, graphView, scene, opaqueItems, maskedItems, compiled.plan,
            preparation, report, frameSequence, &batch);
        context.opaqueGPUDriven = MakeMainSceneGPUDrivenInputs(
            graph, context.identity, culling);
        ASSERT_TRUE(context.opaqueGPUDriven.IsCompatibleWith(context.identity));
        ASSERT_TRUE(context.opaqueGPUDriven.recordedState->Matches(
            GPUCullingRecordingIdentity{
                context.identity.graphIdentity,
                context.identity.graphRecordingGeneration,
                context.identity.frameSequence,
                context.identity.viewOrdinal,
                context.identity.recordEpoch}));
        rayTracedInputs.identity = context.identity;
        executionState->identity = context.identity;
        context.rayTracedShadow = rayTracedInputs;
        context.frameSnapshot = MakeRenderPassFrameSnapshot(
            context, *context.results);
        pass.AddToGraph(graph, context);
    };

    RenderGraph graphA;
    RenderGraph graphB;
    graphA.SetDevice(&device);
    graphB.SetDevice(&device);
    OpaquePass passA;
    OpaquePass passB;
    recordOpaque(graphA, passA, batchA, "OpaqueBatchColorA", "OpaqueBatchMaskA", 624);
    recordOpaque(graphB, passB, batchB, "OpaqueBatchColorB", "OpaqueBatchMaskB", 625);
    graphA.Compile();
    graphB.Compile();
    ASSERT_TRUE(graphA.GetCompileStats().compileValid);
    ASSERT_TRUE(graphB.GetCompileStats().compileValid);

    // Execute B first so the cache's current frame set is B, then replace it
    // while executing A. B must still own its actual bindings until B's token.
    RecordingCommandContext commandsB;
    RecordingCommandContext commandsA;
    graphB.Execute(commandsB);
    ASSERT_FALSE(commandsB.descriptorSetPointers.empty());
    RHIDescriptorSet* const frameSetB = pipelineCache.GetFrameDescriptorSet();
    ASSERT_NE(nullptr, frameSetB);
    EXPECT_NE(std::find(commandsB.descriptorSetPointers.begin(),
                        commandsB.descriptorSetPointers.end(), frameSetB),
              commandsB.descriptorSetPointers.end());
    const std::shared_ptr<FakeDescriptorSetLifetimeState> frameSetBLifetime =
        static_cast<FakeDescriptorSet*>(frameSetB)->lifetime;
    // Each batch owns the frame set, shadow-mask view, color view, and texture.
    EXPECT_EQ(4u, batchB.GetRetainedObjectCount());

    graphA.Execute(commandsA);
    ASSERT_FALSE(commandsA.descriptorSetPointers.empty());
    RHIDescriptorSet* const frameSetA = pipelineCache.GetFrameDescriptorSet();
    ASSERT_NE(nullptr, frameSetA);
    EXPECT_NE(frameSetA, frameSetB);
    EXPECT_NE(std::find(commandsA.descriptorSetPointers.begin(),
                        commandsA.descriptorSetPointers.end(), frameSetA),
              commandsA.descriptorSetPointers.end());
    EXPECT_EQ(4u, batchA.GetRetainedObjectCount());

    GPUCompletionToken completionA;
    GPUCompletionToken completionB;
    ASSERT_TRUE(InsertGPUCompletionPoint(completionA, tracker.Submit(&commandsA)));
    ASSERT_TRUE(InsertGPUCompletionPoint(completionB, tracker.Submit(&commandsB)));
    pipelineCache.RetireOwnerSnapshots(completionA, retirement);
    batchA.SealAndTransfer(completionA, retirement);
    batchB.SealAndTransfer(completionB, retirement);

    FakeFence* const graphicsFence =
        device.FindFenceWithSignal(completionB.points[0].value);
    ASSERT_NE(nullptr, graphicsFence);
    graphicsFence->Complete(completionA.points[0].value);
    EXPECT_EQ(GPUCompletionStatus::Pending, retirement.Poll());
    EXPECT_TRUE(frameSetBLifetime->alive);

    graphicsFence->Complete(completionB.points[0].value);
    EXPECT_EQ(GPUCompletionStatus::Completed, tracker.Query(completionA));
    EXPECT_EQ(GPUCompletionStatus::Completed, tracker.Query(completionB));
    EXPECT_EQ(GPUCompletionStatus::Completed, retirement.Poll());
    EXPECT_FALSE(frameSetBLifetime->alive);
    tracker.Shutdown();
}

TEST_F(RenderPassValidationFixture, RayTracedReflectionPassReusesHistoryAcrossStableFrames)
{
    RVX_REQUIRE_RENDER_RUNTIME_PIPELINE(true, true);

    RayTracingSceneManager rayTracingScene;
    ASSERT_NO_FATAL_FAILURE(PrepareRayTracingSceneForSingleObject(device, gpuResources, scene, rayTracingScene));

    RHITextureDesc colorDesc = RHITextureDesc::RenderTarget(64, 64, RHIFormat::RGBA16_FLOAT);
    colorDesc.debugName = "RayTracedReflectionStableHistoryColor";
    RHITextureRef sceneColorTexture = device.CreateTexture(colorDesc);
    ASSERT_TRUE(sceneColorTexture);

    RHITextureDesc depthDesc = RHITextureDesc::DepthStencil(64, 64, PipelineCache::GetDefaultDepthStencilFormat());
    depthDesc.debugName = "RayTracedReflectionStableHistoryDepth";
    RHITextureRef depthTexture = device.CreateTexture(depthDesc);
    ASSERT_TRUE(depthTexture);

    RayTracedReflectionPass pass;
    pass.SetEnabled(true);
    pass.OnAdd(&device);
    ConfigureResources(pass, gpuResources, pipelineCache, viewCache);
    pass.SetRayTracingScene(&rayTracingScene);

    ASSERT_TRUE(pass.IsSupported()) << pass.GetUnsupportedReason();

    auto runFrame = [&](uint64 frameNumber, RecordingCommandContext& ctx)
    {
        RenderGraph graph;
        graph.SetDevice(&device);

        ViewData frameView;
        frameView.renderGraph = &graph;
        frameView.viewCache = &viewCache;
        frameView.colorTarget = graph.ImportTexture(sceneColorTexture.Get(), RHIResourceState::ShaderResource);
        frameView.depthTarget = graph.ImportTexture(depthTexture.Get(), RHIResourceState::DepthRead);
        frameView.viewportWidth = 64;
        frameView.viewportHeight = 64;
        frameView.aspectRatio = 1.0f;
        frameView.fieldOfView = 1.0472f;
        frameView.nearPlane = 0.1f;
        frameView.farPlane = 100.0f;
        frameView.cameraPosition = Vec3(0.0f, 0.0f, 5.0f);
        frameView.cameraForward = Vec3(0.0f, 0.0f, -1.0f);
        frameView.viewMatrix = Mat4Identity();
        frameView.projectionMatrix = Mat4Identity();
        frameView.viewProjectionMatrix = Mat4Identity();
        frameView.inverseViewMatrix = Mat4Identity();
        frameView.inverseProjectionMatrix = Mat4Identity();
        frameView.previousViewProjectionMatrix = Mat4Identity();
        frameView.frameNumber = frameNumber;
        frameView.resetTemporalHistory = false;

        pass.AddToGraph(graph, frameView);
        graph.Compile();
        EXPECT_TRUE(graph.GetCompileStats().compileValid);
        EXPECT_EQ(graph.GetCompileStats().totalPasses, 1u);

        graph.Execute(ctx);
    };

    RecordingCommandContext firstCtx;
    runFrame(1, firstCtx);
    EXPECT_EQ(firstCtx.dispatchRaysCount, 1u);
    EXPECT_TRUE(firstCtx.lastDispatchRaysValidation.valid) << firstCtx.lastDispatchRaysValidation.message;
    const RayTracedReflectionPassStats firstStats = pass.GetStats();
    EXPECT_TRUE(firstStats.dispatchRecorded);
    EXPECT_TRUE(firstStats.historyRecreated);
    EXPECT_TRUE(firstStats.historyAvailable);
    EXPECT_FALSE(firstStats.temporalAccumulated);
    const size_t textureCountAfterFirstFrame = device.createdTextureDescs.size();

    RecordingCommandContext secondCtx;
    runFrame(2, secondCtx);
    EXPECT_EQ(secondCtx.dispatchRaysCount, 1u);
    EXPECT_TRUE(secondCtx.lastDispatchRaysValidation.valid) << secondCtx.lastDispatchRaysValidation.message;
    const RayTracedReflectionPassStats secondStats = pass.GetStats();
    EXPECT_TRUE(secondStats.dispatchRecorded);
    EXPECT_TRUE(secondStats.historyAvailable);
    EXPECT_TRUE(secondStats.depthHistoryAvailable);
    EXPECT_TRUE(secondStats.normalHistoryAvailable);
    EXPECT_TRUE(secondStats.temporalAccumulated);
    EXPECT_FALSE(secondStats.historyRecreated);
    EXPECT_FALSE(secondStats.historyResolutionChanged);
    EXPECT_FALSE(secondStats.historyConfigChanged);
    EXPECT_FALSE(secondStats.historyReset);
    EXPECT_EQ(device.createdTextureDescs.size(), textureCountAfterFirstFrame);
}

TEST_F(RenderPassValidationFixture, ShadowPassConfigDefaultsToComplementaryRayTracedShadows)
{
    ShadowPassConfig config;

    EXPECT_EQ(config.rayTracedShadowMode, RayTracedShadowMode::ComplementRaster);
    EXPECT_FLOAT_EQ(config.rayTracedHistoryVelocityRejectionScale, 8.0f);
}

TEST_F(RenderPassValidationFixture, ShadowPassRejectsUnsupportedCascadeCounts)
{
    RVX_REQUIRE_RENDER_RUNTIME_PIPELINE();

    ShadowPassConfig config;
    config.numCascades = RVX_MAX_DIRECTIONAL_SHADOW_CASCADES + 1;

    ShadowPass pass;
    ConfigureResources(pass, gpuResources, pipelineCache);
    pass.SetConfig(config);
    pass.SetEnabled(true);

    EXPECT_FALSE(pass.IsSupported());
    EXPECT_NE(pass.GetUnsupportedReason().find("cascade"), std::string::npos);
}

TEST_F(RenderPassValidationFixture, ShadowPassDisabledDoesNotDeclareOrDrawCascadeResources)
{
    RVX_REQUIRE_RENDER_RUNTIME_PIPELINE();

    RenderGraph graph;
    graph.SetDevice(&device);

    view.renderGraph = &graph;
    view.viewCache = &viewCache;
    std::vector<RenderDrawItem> opaqueItems;
    std::vector<RenderDrawItem> maskedItems;
    SceneMeshPassPreparation preparation = PrepareOpaquePackets(
        opaqueItems, maskedItems);
    const RenderFramePlanCompileResult compiled = CompileForcedDirectPlan(
        preparation, 630);
    ASSERT_TRUE(compiled.succeeded);
    RenderFrameExecutionReport report = MakeExecutionReport(compiled.plan);

    ShadowPass pass;
    ConfigureResources(pass, gpuResources, pipelineCache);

    EXPECT_FALSE(pass.IsRequestedEnabled());
    EXPECT_FALSE(pass.IsEnabled());

    const std::shared_ptr<RenderPassRecordResults> results =
        AddTypedShadowRecording(
            graph, pass, view, opaqueItems, maskedItems, compiled.plan,
            preparation, report, MakeShadowPrimaryLight());

    graph.Compile();
    const auto& stats = graph.GetCompileStats();
    EXPECT_TRUE(stats.compileValid);
    EXPECT_EQ(stats.totalPasses, 1u);
    EXPECT_EQ(stats.emptyPassUsageCount, 1u);
    EXPECT_FALSE(results->directionalShadowOutput.enabled);
    EXPECT_EQ(results->shadowStats.declaredCascadeResourceCount, 0u);

    RecordingCommandContext ctx;
    graph.Execute(ctx);

    EXPECT_EQ(ctx.beginRenderPassCount, 0u);
    EXPECT_EQ(ctx.drawIndexedCount, 0u);
    EXPECT_EQ(results->shadowStats.resolvedCascadeViewCount, 0u);
    EXPECT_EQ(results->shadowStats.drawCount, 0u);
}

TEST_F(RenderPassValidationFixture, ShadowPassSetupDeclaresCascadeDepthResourcesAndPSSMMatrices)
{
    RVX_REQUIRE_RENDER_RUNTIME_PIPELINE();

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
    std::vector<RenderDrawItem> opaqueItems;
    std::vector<RenderDrawItem> maskedItems;
    SceneMeshPassPreparation preparation = PrepareOpaquePackets(
        opaqueItems, maskedItems);
    const RenderFramePlanCompileResult compiled = CompileForcedDirectPlan(
        preparation, 631);
    ASSERT_TRUE(compiled.succeeded);
    RenderFrameExecutionReport report = MakeExecutionReport(compiled.plan);

    ShadowPassConfig config;
    config.numCascades = 3;
    config.shadowMapSize = 128;

    ShadowPass pass;
    ConfigureResources(pass, gpuResources, pipelineCache);
    pass.SetConfig(config);
    pass.SetEnabled(true);

    const std::shared_ptr<RenderPassRecordResults> results =
        AddTypedShadowRecording(
            graph, pass, view, opaqueItems, maskedItems, compiled.plan,
            preparation, report, MakeShadowPrimaryLight());

    graph.Compile();
    const DirectionalShadowRecordOutput& output =
        results->directionalShadowOutput;
    ASSERT_TRUE(output.enabled);
    ASSERT_TRUE(output.shadowMap.IsValid());
    EXPECT_EQ(results->shadowStats.configuredCascadeCount, 3u);
    EXPECT_EQ(results->shadowStats.declaredCascadeResourceCount, 3u);

    const RHITextureDesc* desc = graph.GetTextureDesc(output.shadowMap);
    ASSERT_NE(desc, nullptr);
    EXPECT_EQ(desc->width, 128u);
    EXPECT_EQ(desc->height, 128u);
    EXPECT_EQ(desc->arraySize, 3u);
    EXPECT_EQ(desc->format, PipelineCache::GetDefaultDepthStencilFormat());
    EXPECT_TRUE(HasFlag(desc->usage, RHITextureUsage::DepthStencil));

    ASSERT_EQ(output.cascadeViewProjections.size(), static_cast<size_t>(3));
    ASSERT_EQ(output.cascadeSplitDepths.size(), static_cast<size_t>(3));
    float previousSplit = 0.0f;
    for (uint32 i = 0; i < 3u; ++i)
    {
        EXPECT_GT(output.cascadeSplitDepths[i], previousSplit);
        EXPECT_LE(output.cascadeSplitDepths[i], 1.0f);
        EXPECT_FALSE(IsIdentityMatrix(output.cascadeViewProjections[i]));
        previousSplit = output.cascadeSplitDepths[i];
    }

    const auto& stats = graph.GetCompileStats();
    EXPECT_TRUE(stats.compileValid);
    EXPECT_EQ(stats.totalPasses, 1u);
    EXPECT_EQ(stats.culledPasses, 0u);
    EXPECT_EQ(stats.emptyPassUsageCount, 0u);
}

TEST_F(RenderPassValidationFixture, ShadowPassStabilizesCascadeCentersToShadowTexels)
{
    RVX_REQUIRE_RENDER_RUNTIME_PIPELINE();

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
    view.cameraPosition = Vec3(0.37f, 0.0f, 5.19f);
    view.cameraForward = Vec3(0.0f, 0.0f, -1.0f);
    view.inverseViewMatrix = Mat4Identity();
    std::vector<RenderDrawItem> opaqueItems;
    std::vector<RenderDrawItem> maskedItems;
    SceneMeshPassPreparation preparation = PrepareOpaquePackets(
        opaqueItems, maskedItems);
    const RenderFramePlanCompileResult compiled = CompileForcedDirectPlan(
        preparation, 632);
    ASSERT_TRUE(compiled.succeeded);
    RenderFrameExecutionReport stabilizedReport =
        MakeExecutionReport(compiled.plan);

    ShadowPassConfig config;
    config.numCascades = 3;
    config.shadowMapSize = 128;
    config.stabilizeCascades = true;

    ShadowPass stabilizedPass;
    ConfigureResources(stabilizedPass, gpuResources, pipelineCache);
    stabilizedPass.SetConfig(config);
    stabilizedPass.SetEnabled(true);
    const std::shared_ptr<RenderPassRecordResults> stabilizedResults =
        AddTypedShadowRecording(
            graph, stabilizedPass, view, opaqueItems, maskedItems,
            compiled.plan, preparation, stabilizedReport,
            MakeShadowPrimaryLight());
    graph.Compile();
    ASSERT_EQ(stabilizedResults->directionalShadowOutput
                  .cascadeViewProjections.size(),
              static_cast<size_t>(3));

    RenderGraph unsnappedGraph;
    unsnappedGraph.SetDevice(&device);
    view.renderGraph = &unsnappedGraph;
    config.stabilizeCascades = false;
    RenderFrameExecutionReport unsnappedReport =
        MakeExecutionReport(compiled.plan);

    ShadowPass unsnappedPass;
    ConfigureResources(unsnappedPass, gpuResources, pipelineCache);
    unsnappedPass.SetConfig(config);
    unsnappedPass.SetEnabled(true);
    const std::shared_ptr<RenderPassRecordResults> unsnappedResults =
        AddTypedShadowRecording(
            unsnappedGraph, unsnappedPass, view, opaqueItems, maskedItems,
            compiled.plan, preparation, unsnappedReport,
            MakeShadowPrimaryLight());
    unsnappedGraph.Compile();

    ASSERT_EQ(unsnappedResults->directionalShadowOutput
                  .cascadeViewProjections.size(),
              stabilizedResults->directionalShadowOutput
                  .cascadeViewProjections.size());
    EXPECT_NE(0, std::memcmp(
        &stabilizedResults->directionalShadowOutput.cascadeViewProjections[0],
        &unsnappedResults->directionalShadowOutput.cascadeViewProjections[0],
        sizeof(Mat4)));
}

TEST_F(RenderPassValidationFixture, ShadowPassStableCascadeIgnoresSubTexelCameraMotion)
{
    RVX_REQUIRE_RENDER_RUNTIME_PIPELINE();

    auto buildFirstCascadeMatrix = [&](const ViewData& inputView)
    {
        RenderGraph graph;
        graph.SetDevice(&device);

        ViewData localView = inputView;
        localView.renderGraph = &graph;
        localView.viewCache = &viewCache;
        std::vector<RenderDrawItem> opaqueItems;
        std::vector<RenderDrawItem> maskedItems;
        SceneMeshPassPreparation preparation = PrepareOpaquePackets(
            opaqueItems, maskedItems);
        const RenderFramePlanCompileResult compiled = CompileForcedDirectPlan(
            preparation, 633);
        EXPECT_TRUE(compiled.succeeded);
        RenderFrameExecutionReport report = MakeExecutionReport(compiled.plan);

        ShadowPassConfig config;
        config.numCascades = 3;
        config.shadowMapSize = 128;
        config.stabilizeCascades = true;

        ShadowPass pass;
        ConfigureResources(pass, gpuResources, pipelineCache);
        pass.SetConfig(config);
        pass.SetEnabled(true);
        const std::shared_ptr<RenderPassRecordResults> results =
            AddTypedShadowRecording(
                graph, pass, localView, opaqueItems, maskedItems,
                compiled.plan, preparation, report, MakeShadowPrimaryLight());
        graph.Compile();
        EXPECT_FALSE(results->directionalShadowOutput
                         .cascadeViewProjections.empty());
        return results->directionalShadowOutput.cascadeViewProjections[0];
    };

    ViewData baseView = view;
    baseView.viewportWidth = 320;
    baseView.viewportHeight = 180;
    baseView.aspectRatio = 16.0f / 9.0f;
    baseView.fieldOfView = 1.0472f;
    baseView.nearPlane = 0.1f;
    baseView.farPlane = 100.0f;
    baseView.cameraPosition = Vec3(0.37f, 0.0f, 5.19f);
    baseView.cameraForward = Vec3(0.0f, 0.0f, -1.0f);
    baseView.inverseViewMatrix = Mat4Identity();

    ShadowCascade baseCascade;
    baseCascade.viewProjection = buildFirstCascadeMatrix(baseView);
    constexpr uint32 shadowMapSize = 128;
    const float rowScale = std::sqrt(
        baseCascade.viewProjection[0][0] * baseCascade.viewProjection[0][0] +
        baseCascade.viewProjection[1][0] * baseCascade.viewProjection[1][0] +
        baseCascade.viewProjection[2][0] * baseCascade.viewProjection[2][0]);
    ASSERT_GT(rowScale, 0.0f);
    const float texelWorldSize = 2.0f /
        (rowScale * static_cast<float>(shadowMapSize));
    const Vec3 fixedWorldPoint = baseView.cameraPosition +
        baseView.cameraForward * 1.0f;
    const Vec2 baseShadowUV = ProjectShadowUV(baseCascade, fixedWorldPoint);

    ViewData smallMoveView = baseView;
    smallMoveView.cameraPosition.x += texelWorldSize * 0.1f;
    ShadowCascade smallMoveCascade;
    smallMoveCascade.viewProjection = buildFirstCascadeMatrix(smallMoveView);
    const Vec2 smallMoveShadowUV = ProjectShadowUV(
        smallMoveCascade, fixedWorldPoint);
    EXPECT_NEAR(smallMoveShadowUV.x, baseShadowUV.x, 0.00001f);
    EXPECT_NEAR(smallMoveShadowUV.y, baseShadowUV.y, 0.00001f);

    ViewData largeMoveView = baseView;
    largeMoveView.cameraPosition.x += texelWorldSize * 8.0f;
    ShadowCascade largeMoveCascade;
    largeMoveCascade.viewProjection = buildFirstCascadeMatrix(largeMoveView);
    const Vec2 largeMoveShadowUV = ProjectShadowUV(
        largeMoveCascade, fixedWorldPoint);
    EXPECT_GT(length(largeMoveShadowUV - baseShadowUV), 0.001f);
}

TEST_F(RenderPassValidationFixture, ShadowPassSingleCascadeStillDeclaresArrayCompatibleTexture)
{
    RVX_REQUIRE_RENDER_RUNTIME_PIPELINE();

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
    std::vector<RenderDrawItem> opaqueItems;
    std::vector<RenderDrawItem> maskedItems;
    SceneMeshPassPreparation preparation = PrepareOpaquePackets(
        opaqueItems, maskedItems);
    const RenderFramePlanCompileResult compiled = CompileForcedDirectPlan(
        preparation, 634);
    ASSERT_TRUE(compiled.succeeded);
    RenderFrameExecutionReport report = MakeExecutionReport(compiled.plan);

    ShadowPassConfig config;
    config.numCascades = 1;
    config.shadowMapSize = 64;

    ShadowPass pass;
    ConfigureResources(pass, gpuResources, pipelineCache);
    pass.SetConfig(config);
    pass.SetEnabled(true);

    const std::shared_ptr<RenderPassRecordResults> results =
        AddTypedShadowRecording(
            graph, pass, view, opaqueItems, maskedItems, compiled.plan,
            preparation, report, MakeShadowPrimaryLight());
    graph.Compile();

    ASSERT_TRUE(results->directionalShadowOutput.shadowMap.IsValid());
    const RHITextureDesc* desc = graph.GetTextureDesc(
        results->directionalShadowOutput.shadowMap);
    ASSERT_NE(desc, nullptr);
    EXPECT_EQ(desc->arraySize, RVX_MIN_DIRECTIONAL_SHADOW_ARRAY_LAYERS);
    ASSERT_EQ(results->directionalShadowOutput.cascadeViewProjections.size(),
              static_cast<size_t>(1));
}

TEST_F(RenderPassValidationFixture, ShadowPassExecuteResolvesCascadeViewsAndDrawsOnlyShadowCasters)
{
    RVX_REQUIRE_RENDER_RUNTIME_PIPELINE();

    RenderObject nonCaster = MakeRenderObject(*meshResource, gpuResources);
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
    std::vector<RenderDrawItem> opaqueItems;
    std::vector<RenderDrawItem> maskedItems;
    SceneMeshPassPreparation preparation = PrepareOpaquePackets(
        opaqueItems, maskedItems);
    const RenderFramePlanCompileResult compiled = CompileForcedDirectPlan(
        preparation, 635);
    ASSERT_TRUE(compiled.succeeded);
    RenderFrameExecutionReport report = MakeExecutionReport(compiled.plan);

    ShadowPassConfig config;
    config.numCascades = 2;
    config.shadowMapSize = 64;
    config.casterDepthBias = 1.25f;
    config.casterSlopeScaledDepthBias = 2.0f;
    config.casterDepthBiasClamp = 0.5f;

    ShadowPass pass;
    ConfigureResources(pass, gpuResources, pipelineCache);
    pass.SetConfig(config);
    pass.SetEnabled(true);

    const std::shared_ptr<RenderPassRecordResults> results =
        AddTypedShadowRecording(
            graph, pass, view, opaqueItems, maskedItems, compiled.plan,
            preparation, report, MakeShadowPrimaryLight());
    graph.Compile();

    RecordingCommandContext ctx;
    graph.Execute(ctx);

    EXPECT_EQ(ctx.beginRenderPassCount, 2u);
    EXPECT_EQ(ctx.endRenderPassCount, 2u);
    EXPECT_EQ(ctx.drawIndexedCount, 2u);
    EXPECT_EQ(results->shadowStats.resolvedCascadeViewCount, 2u);
    EXPECT_EQ(results->shadowStats.shadowCasterCount, 2u);
    EXPECT_EQ(results->shadowStats.drawCount, 2u);
    EXPECT_EQ(ctx.depthBiasSetCount, 0u);
    RHIPipeline* shadowPipeline = pipelineCache.GetShadowDepthPipeline(
        ShadowDepthBiasState{config.casterDepthBias,
                             config.casterSlopeScaledDepthBias,
                             config.casterDepthBiasClamp});
    ASSERT_NE(shadowPipeline, nullptr);
    EXPECT_NE(shadowPipeline, pipelineCache.GetDepthOnlyPipeline());
    ASSERT_EQ(ctx.pipelineSequence.size(), static_cast<size_t>(2));
    EXPECT_EQ(ctx.pipelineSequence[0], shadowPipeline);
    EXPECT_EQ(ctx.pipelineSequence[1], shadowPipeline);
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

TEST_F(RenderPassValidationFixture, SkyboxPassWithoutFrameSnapshotDoesNotBindOrDraw)
{
    RVX_REQUIRE_RENDER_RUNTIME_PIPELINE();

    SkyboxPass pass;
    pass.SetResources(&pipelineCache);

    EXPECT_TRUE(pass.IsRequestedEnabled());
    EXPECT_TRUE(pass.IsSupported()) << pass.GetUnsupportedReason();
    EXPECT_TRUE(pass.IsEnabled());

    RecordingCommandContext ctx;
    pass.Execute(ctx, view);

    EXPECT_EQ(ctx.beginRenderPassCount, 0u);
    EXPECT_TRUE(ctx.pipelineSequence.empty());
    EXPECT_EQ(ctx.drawCount, 0u);
}

TEST_F(RenderPassValidationFixture,
       SkyboxRegistrySupportIsIndependentOfTypedFrameSelection)
{
    RVX_REQUIRE_RENDER_RUNTIME_PIPELINE();

    RenderPassRegistry registry;
    auto skybox = std::make_unique<SkyboxPass>();
    skybox->SetResources(&pipelineCache);
    skybox->SetResourceRegistry(&gpuResources.GetRegistry());
    registry.AddPass(std::move(skybox), &device);

    std::vector<RenderPassStatus> statuses = registry.GetPassStatuses();
    ASSERT_EQ(1u, statuses.size());
    EXPECT_TRUE(statuses[0].requestedEnabled);
    EXPECT_TRUE(statuses[0].supported) << statuses[0].unsupportedReason;
    EXPECT_TRUE(statuses[0].enabled) << statuses[0].unsupportedReason;

    auto* const pass = static_cast<SkyboxPass*>(registry.GetPasses()[0].get());
    ASSERT_NE(nullptr, pass);
    RenderGraph graph;
    graph.SetDevice(&device);
    ViewData recordView = view;
    recordView.renderGraph = &graph;
    recordView.viewCache = &viewCache;
    recordView.colorTarget = graph.ImportTexture(
        colorTexture.Get(), RHIResourceState::RenderTarget);
    RenderSkySnapshot disabledSky;
    disabledSky.mode = RenderSkyMode::Disabled;
    RenderPassRecordContext context = MakeSkyboxRecordContext(
        graph, recordView, scene, disabledSky, 900, 900);
    pass->AddToGraph(graph, context);
    graph.Compile();
    ASSERT_TRUE(graph.GetCompileStats().compileValid);
    ASSERT_EQ(1u, graph.GetDiagnostics().passes.size());
    EXPECT_TRUE(graph.GetDiagnostics().passes[0].usages.empty());

    statuses = registry.GetPassStatuses();
    ASSERT_EQ(1u, statuses.size());
    EXPECT_TRUE(statuses[0].supported);
    EXPECT_TRUE(statuses[0].enabled);
}

TEST_F(RenderPassValidationFixture, SkyboxPassDrawsProceduralFullscreenTriangleThroughRenderGraph)
{
    RVX_REQUIRE_RENDER_RUNTIME_PIPELINE();

    RenderGraph graph;
    graph.SetDevice(&device);

    RHITextureRef graphColor = device.CreateTexture(
        RHITextureDesc::RenderTarget(64, 64, RHIFormat::RGBA8_UNORM));
    RHITextureRef graphDepth = device.CreateTexture(
        RHITextureDesc::DepthStencil(
            64, 64, PipelineCache::GetDefaultDepthStencilFormat()));
    ASSERT_TRUE(graphColor);
    ASSERT_TRUE(graphDepth);
    view.colorTarget = graph.ImportTexture(
        graphColor.Get(), RHIResourceState::RenderTarget);
    view.depthTarget = graph.ImportTexture(
        graphDepth.Get(), RHIResourceState::DepthRead);

    view.renderGraph = &graph;
    view.viewCache = &viewCache;
    view.viewportWidth = 64;
    view.viewportHeight = 64;

    SkyboxPass pass;
    pass.SetResources(&pipelineCache);
    pass.SetResourceRegistry(&gpuResources.GetRegistry());

    ASSERT_TRUE(pass.IsSupported()) << pass.GetUnsupportedReason();
    ASSERT_TRUE(pass.IsEnabled());

    RenderSkySnapshot sky;
    sky.mode = RenderSkyMode::Procedural;
    sky.tint = Vec3{0.12f, 0.24f, 0.55f};
    sky.sunDirection = Vec3{0.25f, 0.8f, 0.35f};
    sky.sunColor = Vec3{0.95f, 0.8f, 0.65f};
    sky.zenithColor = Vec3{0.12f, 0.24f, 0.55f};
    sky.horizonColor = Vec3{0.6f, 0.72f, 0.88f};
    sky.groundColor = Vec3{0.1f, 0.12f, 0.16f};
    sky.intensity = 1.25f;
    sky.scatteringIntensity = 1.5f;
    RenderPassRecordContext context = MakeSkyboxRecordContext(
        graph, view, scene, sky, 901, 901);
    pass.AddToGraph(graph, context);

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
            return desc.debugName &&
                   std::string(desc.debugName) == "SkyboxRecordDescriptorSet";
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
    ASSERT_EQ(device.createdDescriptorSetDescs.size(),
              device.createdDescriptorSets.size());
    const size_t descriptorIndex = static_cast<size_t>(
        std::distance(device.createdDescriptorSetDescs.begin(), descriptorIt));
    EXPECT_EQ(pipelineCache.GetSkyboxSetLayout(), descriptorIt->layout);
    EXPECT_EQ(pipelineCache.GetSkyboxSetLayout(),
              device.createdDescriptorSets[descriptorIndex]->GetLayoutIdentity());
    EXPECT_TRUE(device.createdDescriptorSets[descriptorIndex]->IsReadyForBinding(
        pipelineCache.GetSkyboxSetLayout()));

    struct SkyboxConstantsProbe
    {
        float zenithColor[4];
        float horizonColor[4];
        float groundColor[4];
        float sunDirection[4];
        float sunColor[4];
        float textureParams[4];
    };
    const FakeBuffer* constantsBuffer = FindCreatedBuffer(
        device, "SkyboxRecordConstants");
    ASSERT_NE(constantsBuffer, nullptr);
    SkyboxConstantsProbe constants{};
    std::memcpy(&constants,
                constantsBuffer->GetStorage().data(),
                sizeof(constants));
    EXPECT_FLOAT_EQ(0.12f, constants.zenithColor[0]);
    EXPECT_FLOAT_EQ(1.25f, constants.zenithColor[3]);
    EXPECT_FLOAT_EQ(0.6f, constants.horizonColor[0]);
    EXPECT_FLOAT_EQ(1.5f, constants.horizonColor[3]);
    EXPECT_FLOAT_EQ(0.1f, constants.groundColor[0]);
    EXPECT_FLOAT_EQ(0.999f, constants.groundColor[3]);
    EXPECT_FLOAT_EQ(0.25f, constants.sunDirection[0]);
    EXPECT_FLOAT_EQ(1.0f, constants.sunDirection[3]);
    EXPECT_FLOAT_EQ(0.95f, constants.sunColor[0]);
    EXPECT_FLOAT_EQ(0.0f, constants.textureParams[0]);
}

TEST_F(RenderPassValidationFixture, SkyboxPassDrawsFullscreenBackgroundWithoutDepthTarget)
{
    RVX_REQUIRE_RENDER_RUNTIME_PIPELINE();

    RenderGraph graph;
    graph.SetDevice(&device);
    view.colorTarget = graph.ImportTexture(colorTexture.Get(), RHIResourceState::RenderTarget);
    view.renderGraph = &graph;
    view.viewCache = &viewCache;
    view.viewportWidth = 64;
    view.viewportHeight = 64;

    SkyboxPass pass;
    pass.SetResources(&pipelineCache);
    pass.SetResourceRegistry(&gpuResources.GetRegistry());

    RenderSkySnapshot sky;
    sky.mode = RenderSkyMode::Procedural;
    sky.tint = Vec3{0.12f, 0.24f, 0.55f};
    RenderPassRecordContext context = MakeSkyboxRecordContext(
        graph, view, scene, sky, 902, 902);
    pass.AddToGraph(graph, context);
    graph.Compile();

    RecordingCommandContext ctx;
    graph.Execute(ctx);

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
    RVX_REQUIRE_RENDER_RUNTIME_PIPELINE();

    RenderGraph graph;
    graph.SetDevice(&device);
    view.colorTarget = graph.ImportTexture(colorTexture.Get(), RHIResourceState::RenderTarget);
    view.renderGraph = &graph;
    view.viewportWidth = 64;
    view.viewportHeight = 64;
    view.viewCache = &viewCache;

    Resource::TextureHandle cubemapResource = CreateCubemapTextureResource(910);
    const RenderResourceHandle cubemapHandle =
        gpuResources.ResolveOrUpload(cubemapResource.Get());
    ASSERT_TRUE(cubemapHandle.IsValid());
    RHITexture* cubemap = gpuResources.GetRegistry().ResolveTextureObject(cubemapHandle);
    ASSERT_NE(cubemap, nullptr);

    SkyboxPass pass;
    pass.SetResources(&pipelineCache);
    pass.SetResourceRegistry(&gpuResources.GetRegistry());

    ASSERT_TRUE(pass.IsSupported()) << pass.GetUnsupportedReason();

    RenderSkySnapshot sky;
    sky.mode = RenderSkyMode::Cubemap;
    sky.skyTexture = cubemapHandle;
    sky.intensity = 1.25f;
    sky.rotationRadians = 0.35f;
    sky.blurLevel = 1.0f;
    RenderPassRecordContext context = MakeSkyboxRecordContext(
        graph, view, scene, sky, 903, 903);
    pass.AddToGraph(graph, context);
    graph.Compile();

    RecordingCommandContext ctx;
    graph.Execute(ctx);

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
            return desc.debugName &&
                   std::string(desc.debugName) == "SkyboxRecordDescriptorSet";
        });
    ASSERT_NE(descriptorIt, device.createdDescriptorSetDescs.end());
    ASSERT_EQ(descriptorIt->bindings.size(), static_cast<size_t>(3));
    EXPECT_EQ(descriptorIt->bindings[0].binding, 0u);
    EXPECT_NE(descriptorIt->bindings[0].buffer, nullptr);
    EXPECT_EQ(descriptorIt->bindings[1].binding, 1u);
    ASSERT_NE(descriptorIt->bindings[1].textureView, nullptr);
    EXPECT_EQ(descriptorIt->bindings[1].textureView->GetTexture(), cubemap);
    EXPECT_EQ(descriptorIt->bindings[2].binding, 2u);
    EXPECT_NE(descriptorIt->bindings[2].sampler, nullptr);
}

TEST_F(RenderPassValidationFixture, SkyboxPassFallsBackToTintWhenCubemapSRVCreationFails)
{
    RVX_REQUIRE_RENDER_RUNTIME_PIPELINE();

    RenderGraph graph;
    graph.SetDevice(&device);
    view.colorTarget = graph.ImportTexture(colorTexture.Get(), RHIResourceState::RenderTarget);
    view.renderGraph = &graph;
    view.viewCache = &viewCache;

    Resource::TextureHandle cubemapResource = CreateCubemapTextureResource(911);
    const RenderResourceHandle cubemapHandle =
        gpuResources.ResolveOrUpload(cubemapResource.Get());
    ASSERT_TRUE(cubemapHandle.IsValid());
    RHITexture* cubemap = gpuResources.GetRegistry().ResolveTextureObject(cubemapHandle);
    ASSERT_NE(cubemap, nullptr);

    SkyboxPass pass;
    pass.SetResources(&pipelineCache);
    pass.SetResourceRegistry(&gpuResources.GetRegistry());
    // SetResources establishes the immutable fallback before the selected
    // cubemap SRV path is made to fail.
    ASSERT_TRUE(pass.IsSupported()) << pass.GetUnsupportedReason();

    ASSERT_NE(viewCache.GetDefaultRTV(colorTexture.Get()), nullptr);

    device.textureViewCreationSucceeds = false;

    RenderSkySnapshot sky;
    sky.mode = RenderSkyMode::Cubemap;
    sky.skyTexture = cubemapHandle;
    sky.tint = Vec3{0.2f, 0.4f, 0.7f};
    sky.intensity = 0.8f;
    RenderPassRecordContext context = MakeSkyboxRecordContext(
        graph, view, scene, sky, 904, 904);
    pass.AddToGraph(graph, context);
    graph.Compile();

    RecordingCommandContext ctx;
    graph.Execute(ctx);

    // A valid handle whose SRV cannot be resolved is a solid fallback, not a
    // cross-frame failure or a direct read of SetCubemap state.
    EXPECT_EQ(1u, ctx.drawCount);
    ASSERT_EQ(1u, ctx.renderPasses.size());
    const auto descriptorIt = std::find_if(
        device.createdDescriptorSetDescs.begin(),
        device.createdDescriptorSetDescs.end(),
        [](const RHIDescriptorSetDesc& desc)
        {
            return desc.debugName &&
                   std::string(desc.debugName) == "SkyboxRecordDescriptorSet";
        });
    ASSERT_NE(descriptorIt, device.createdDescriptorSetDescs.end());
    ASSERT_GE(descriptorIt->bindings.size(), static_cast<size_t>(2));
    EXPECT_NE(descriptorIt->bindings[1].textureView->GetTexture(), cubemap);
}

TEST_F(RenderPassValidationFixture,
       SkyboxPassUsesTintFallbackForEquirectangularSnapshot)
{
    RVX_REQUIRE_RENDER_RUNTIME_PIPELINE();

    RenderGraph graph;
    graph.SetDevice(&device);
    view.colorTarget = graph.ImportTexture(
        colorTexture.Get(), RHIResourceState::RenderTarget);
    view.renderGraph = &graph;
    view.viewCache = &viewCache;

    SkyboxPass pass;
    pass.SetResources(&pipelineCache);
    pass.SetResourceRegistry(&gpuResources.GetRegistry());
    ASSERT_TRUE(pass.IsSupported()) << pass.GetUnsupportedReason();

    RenderSkySnapshot sky;
    sky.mode = RenderSkyMode::Equirectangular;
    sky.skyTexture = RenderResourceHandle{900, 1};
    sky.tint = Vec3{0.3f, 0.45f, 0.6f};
    sky.intensity = 0.75f;
    RenderPassRecordContext context = MakeSkyboxRecordContext(
        graph, view, scene, sky, 907, 907);
    pass.AddToGraph(graph, context);
    graph.Compile();

    RecordingCommandContext commands;
    graph.Execute(commands);
    ASSERT_EQ(1u, commands.drawCount);

    const FakeBuffer* constantsBuffer = FindCreatedBuffer(
        device, "SkyboxRecordConstants");
    ASSERT_NE(constantsBuffer, nullptr);
    struct EquirectangularFallbackConstants
    {
        float zenithColor[4];
        float horizonColor[4];
        float groundColor[4];
        float sunDirection[4];
        float sunColor[4];
        float textureParams[4];
    } constants{};
    std::memcpy(&constants,
                constantsBuffer->GetStorage().data(),
                sizeof(constants));
    EXPECT_FLOAT_EQ(0.3f, constants.zenithColor[0]);
    EXPECT_FLOAT_EQ(0.75f, constants.zenithColor[3]);
    EXPECT_FLOAT_EQ(0.45f, constants.horizonColor[1]);
    EXPECT_FLOAT_EQ(0.0f, constants.horizonColor[3]);
    EXPECT_FLOAT_EQ(0.0f, constants.textureParams[0]);
}

TEST_F(RenderPassValidationFixture, SkyboxPassSkipsDrawWhenSamplerCannotBeCreated)
{
    RVX_REQUIRE_RENDER_RUNTIME_PIPELINE();

    RenderGraph graph;
    graph.SetDevice(&device);
    view.colorTarget = graph.ImportTexture(colorTexture.Get(), RHIResourceState::RenderTarget);
    view.renderGraph = &graph;
    view.viewCache = &viewCache;
    device.samplerCreationSucceeds = false;

    SkyboxPass pass;
    pass.SetResources(&pipelineCache);
    pass.SetResourceRegistry(&gpuResources.GetRegistry());

    EXPECT_FALSE(pass.IsSupported());

    RenderSkySnapshot sky;
    sky.mode = RenderSkyMode::Procedural;
    RenderPassRecordContext context = MakeSkyboxRecordContext(
        graph, view, scene, sky, 905, 905);
    pass.AddToGraph(graph, context);
    graph.Compile();

    RecordingCommandContext ctx;
    graph.Execute(ctx);

    EXPECT_TRUE(ctx.pipelineSequence.empty());
    EXPECT_EQ(ctx.drawCount, 0u);
    const RenderGraph::Diagnostics diagnostics = graph.GetDiagnostics();
    ASSERT_EQ(1u, diagnostics.passes.size());
    EXPECT_TRUE(diagnostics.passes[0].usages.empty());
}

TEST_F(RenderPassValidationFixture, SkyboxPassSkipsDrawWhenConstantsCannotMap)
{
    RVX_REQUIRE_RENDER_RUNTIME_PIPELINE(false);

    RenderGraph graph;
    graph.SetDevice(&device);
    view.colorTarget = graph.ImportTexture(colorTexture.Get(), RHIResourceState::RenderTarget);
    view.renderGraph = &graph;
    view.viewCache = &viewCache;

    SkyboxPass pass;
    pass.SetResources(&pipelineCache);
    pass.SetResourceRegistry(&gpuResources.GetRegistry());

    RenderSkySnapshot sky;
    sky.mode = RenderSkyMode::Procedural;
    RenderPassRecordContext context = MakeSkyboxRecordContext(
        graph, view, scene, sky, 906, 906);
    pass.AddToGraph(graph, context);
    graph.Compile();

    RecordingCommandContext ctx;
    graph.Execute(ctx);

    EXPECT_TRUE(ctx.pipelineSequence.empty());
    EXPECT_EQ(ctx.drawCount, 0u);
    const RenderGraph::Diagnostics diagnostics = graph.GetDiagnostics();
    ASSERT_EQ(1u, diagnostics.passes.size());
    EXPECT_TRUE(diagnostics.passes[0].usages.empty());
}

TEST_F(RenderPassValidationFixture,
       SkyboxPassTypedRecordingOwnsReverseSnapshotsConstantsAndAttachments)
{
    RVX_REQUIRE_RENDER_RUNTIME_PIPELINE(true, false, true);

    struct SkyboxConstantsProbe
    {
        float zenithColor[4];
        float horizonColor[4];
        float groundColor[4];
        float sunDirection[4];
        float sunColor[4];
        float textureParams[4];
        float cameraPosition[4];
        Mat4 inverseViewProjection;
    };

    const auto makeTargets = [this](uint32 extent, bool withDepth)
    {
        RHITextureRef color = device.CreateTexture(
            RHITextureDesc::RenderTarget(extent, extent, RHIFormat::RGBA8_UNORM));
        RHITextureRef depth = withDepth ? device.CreateTexture(
            RHITextureDesc::DepthStencil(
                extent, extent, PipelineCache::GetDefaultDepthStencilFormat()))
            : RHITextureRef{};
        return std::pair<RHITextureRef, RHITextureRef>(std::move(color), std::move(depth));
    };
    auto targetsA = makeTargets(64, true);
    auto targetsB = makeTargets(128, false);
    ASSERT_TRUE(targetsA.first);
    ASSERT_TRUE(targetsA.second);
    ASSERT_TRUE(targetsB.first);

    Resource::TextureHandle cubemapResource = CreateCubemapTextureResource(912);
    const RenderResourceHandle cubemapHandle =
        gpuResources.ResolveOrUpload(cubemapResource.Get());
    ASSERT_TRUE(cubemapHandle.IsValid());
    RHITexture* cubemap = gpuResources.GetRegistry().ResolveTextureObject(cubemapHandle);
    ASSERT_NE(cubemap, nullptr);

    SkyboxPass pass;
    pass.SetResources(&pipelineCache);
    pass.SetResourceRegistry(&gpuResources.GetRegistry());
    ASSERT_TRUE(pass.IsSupported()) << pass.GetUnsupportedReason();

    RenderGraph graphA;
    RenderGraph graphB;
    graphA.SetDevice(&device);
    graphB.SetDevice(&device);
    ViewData viewA = view;
    viewA.renderGraph = &graphA;
    viewA.viewCache = &viewCache;
    viewA.colorTarget = graphA.ImportTexture(
        targetsA.first.Get(), RHIResourceState::RenderTarget);
    viewA.depthTarget = graphA.ImportTexture(
        targetsA.second.Get(), RHIResourceState::DepthRead);
    viewA.viewportWidth = 64;
    viewA.viewportHeight = 64;
    viewA.cameraPosition = Vec3{2.0f, 3.0f, 4.0f};
    ViewData viewB = view;
    viewB.renderGraph = &graphB;
    viewB.viewCache = &viewCache;
    viewB.colorTarget = graphB.ImportTexture(
        targetsB.first.Get(), RHIResourceState::RenderTarget);
    viewB.viewportWidth = 128;
    viewB.viewportHeight = 128;
    viewB.cameraPosition = Vec3{9.0f, 8.0f, 7.0f};

    RenderSkySnapshot skyA;
    skyA.mode = RenderSkyMode::SolidColor;
    skyA.tint = Vec3{0.15f, 0.25f, 0.35f};
    skyA.intensity = 1.5f;
    RenderSkySnapshot skyB;
    skyB.mode = RenderSkyMode::Cubemap;
    skyB.skyTexture = cubemapHandle;
    skyB.tint = Vec3{0.8f, 0.7f, 0.6f};
    skyB.intensity = 0.75f;
    skyB.rotationRadians = 0.55f;
    skyB.blurLevel = 2.0f;
    RenderPassRecordContext contextA = MakeSkyboxRecordContext(
        graphA, viewA, scene, skyA, 920, 920);
    RenderPassRecordContext contextB = MakeSkyboxRecordContext(
        graphB, viewB, scene, skyB, 921, 921);
    pass.AddToGraph(graphA, contextA);
    pass.AddToGraph(graphB, contextB);

    // Neither later pass enablement nor a caller replacement of the source
    // context can change already-registered graph recording input.
    pass.SetEnabled(false);
    auto replacementSnapshot = std::make_shared<RenderPassFrameSnapshot>(
        *contextA.frameSnapshot);
    replacementSnapshot->sky.tint = Vec3{9.0f, 9.0f, 9.0f};
    replacementSnapshot->view.renderFrameExecutionPlan =
        &replacementSnapshot->executionPlan;
    replacementSnapshot->view.meshPassPreparation =
        &replacementSnapshot->meshPassPreparation;
    replacementSnapshot->view.renderVisibility = &replacementSnapshot->visibility;
    replacementSnapshot->view.renderFrameExecutionReport =
        &contextA.results->executionReport;
    contextA.frameSnapshot = std::move(replacementSnapshot);

    graphA.Compile();
    graphB.Compile();
    ASSERT_TRUE(graphA.GetCompileStats().compileValid);
    ASSERT_TRUE(graphB.GetCompileStats().compileValid);
    const auto expectAccesses = [](const RenderGraph& graph,
                                   RGTextureHandle color,
                                   RGTextureHandle depth,
                                   bool expectsDepth)
    {
        const RenderGraph::Diagnostics diagnostics = graph.GetDiagnostics();
        ASSERT_EQ(1u, diagnostics.passes.size());
        const auto colorUsage = std::find_if(
            diagnostics.passes[0].usages.begin(), diagnostics.passes[0].usages.end(),
            [color](const RenderGraph::ResourceUsageDiagnostic& usage)
            {
                return usage.type == RenderGraph::DiagnosticResourceType::Texture &&
                       usage.resourceIndex == color.index;
            });
        ASSERT_NE(diagnostics.passes[0].usages.end(), colorUsage);
        EXPECT_EQ(RenderGraph::DiagnosticAccessType::ReadWrite, colorUsage->access);
        EXPECT_EQ(RHIResourceState::RenderTarget, colorUsage->desiredState);
        if (expectsDepth)
        {
            const auto depthUsage = std::find_if(
                diagnostics.passes[0].usages.begin(), diagnostics.passes[0].usages.end(),
                [depth](const RenderGraph::ResourceUsageDiagnostic& usage)
                {
                    return usage.type == RenderGraph::DiagnosticResourceType::Texture &&
                           usage.resourceIndex == depth.index;
                });
            ASSERT_NE(diagnostics.passes[0].usages.end(), depthUsage);
            EXPECT_EQ(RenderGraph::DiagnosticAccessType::Read, depthUsage->access);
            EXPECT_EQ(RHIResourceState::DepthRead, depthUsage->desiredState);
        }
    };
    expectAccesses(graphA, contextA.view.colorTarget, contextA.view.depthTarget, true);
    expectAccesses(graphB, contextB.view.colorTarget, contextB.view.depthTarget, false);

    std::vector<FakeBuffer*> recordConstants;
    for (size_t index = 0; index < device.createdBuffers.size(); ++index)
    {
        const char* name = device.createdBufferDescs[index].debugName;
        if (name && std::string(name) == "SkyboxRecordConstants")
        {
            recordConstants.push_back(device.createdBuffers[index]);
        }
    }
    ASSERT_EQ(2u, recordConstants.size());
    SkyboxConstantsProbe constantsA{};
    SkyboxConstantsProbe constantsB{};
    std::memcpy(&constantsA, recordConstants[0]->GetStorage().data(), sizeof(constantsA));
    std::memcpy(&constantsB, recordConstants[1]->GetStorage().data(), sizeof(constantsB));
    EXPECT_FLOAT_EQ(0.15f, constantsA.zenithColor[0]);
    EXPECT_FLOAT_EQ(1.5f, constantsA.zenithColor[3]);
    EXPECT_FLOAT_EQ(0.0f, constantsA.textureParams[0]);
    EXPECT_FLOAT_EQ(0.001f, constantsA.groundColor[3]);
    EXPECT_FLOAT_EQ(2.0f, constantsA.cameraPosition[0]);
    EXPECT_FLOAT_EQ(1.0f, constantsB.textureParams[0]);
    EXPECT_FLOAT_EQ(2.0f, constantsB.textureParams[1]);
    EXPECT_FLOAT_EQ(0.55f, constantsB.textureParams[2]);
    EXPECT_FLOAT_EQ(9.0f, constantsB.cameraPosition[0]);

    std::vector<size_t> recordDescriptorIndices;
    for (size_t index = 0; index < device.createdDescriptorSetDescs.size(); ++index)
    {
        const char* name = device.createdDescriptorSetDescs[index].debugName;
        if (name && std::string(name) == "SkyboxRecordDescriptorSet")
        {
            recordDescriptorIndices.push_back(index);
        }
    }
    ASSERT_EQ(2u, recordDescriptorIndices.size());
    const RHIDescriptorSetDesc& descriptorA =
        device.createdDescriptorSetDescs[recordDescriptorIndices[0]];
    const RHIDescriptorSetDesc& descriptorB =
        device.createdDescriptorSetDescs[recordDescriptorIndices[1]];
    ASSERT_EQ(3u, descriptorA.bindings.size());
    ASSERT_EQ(3u, descriptorB.bindings.size());
    EXPECT_EQ(0u, descriptorA.bindings[0].binding);
    EXPECT_EQ(0u, descriptorB.bindings[0].binding);
    EXPECT_NE(descriptorA.bindings[0].buffer, descriptorB.bindings[0].buffer);
    EXPECT_EQ(1u, descriptorA.bindings[1].binding);
    EXPECT_EQ(1u, descriptorB.bindings[1].binding);
    ASSERT_NE(descriptorA.bindings[1].textureView, nullptr);
    ASSERT_NE(descriptorB.bindings[1].textureView, nullptr);
    EXPECT_NE(cubemap, descriptorA.bindings[1].textureView->GetTexture());
    EXPECT_EQ(cubemap, descriptorB.bindings[1].textureView->GetTexture());
    EXPECT_EQ(2u, descriptorA.bindings[2].binding);
    EXPECT_EQ(2u, descriptorB.bindings[2].binding);
    EXPECT_NE(descriptorA.bindings[2].sampler, nullptr);
    EXPECT_NE(descriptorB.bindings[2].sampler, nullptr);
    EXPECT_EQ(pipelineCache.GetSkyboxSetLayout(), descriptorA.layout);
    EXPECT_EQ(pipelineCache.GetSkyboxSetLayout(), descriptorB.layout);
    EXPECT_TRUE(device.createdDescriptorSets[recordDescriptorIndices[0]]
                    ->IsReadyForBinding(pipelineCache.GetSkyboxSetLayout()));
    EXPECT_TRUE(device.createdDescriptorSets[recordDescriptorIndices[1]]
                    ->IsReadyForBinding(pipelineCache.GetSkyboxSetLayout()));

    RecordingCommandContext commandsB;
    RecordingCommandContext commandsA;
    graphB.Execute(commandsB);
    graphA.Execute(commandsA);
    ASSERT_EQ(1u, commandsA.drawCount);
    ASSERT_EQ(1u, commandsB.drawCount);
    ASSERT_EQ(1u, commandsA.renderPasses.size());
    ASSERT_EQ(1u, commandsB.renderPasses.size());
    EXPECT_EQ(targetsA.first.Get(),
              commandsA.renderPasses[0].colorAttachments[0].view->GetTexture());
    EXPECT_EQ(targetsB.first.Get(),
              commandsB.renderPasses[0].colorAttachments[0].view->GetTexture());
    EXPECT_EQ(RHILoadOp::Load,
              commandsA.renderPasses[0].colorAttachments[0].loadOp);
    EXPECT_EQ(RHIStoreOp::Store,
              commandsA.renderPasses[0].colorAttachments[0].storeOp);
    EXPECT_TRUE(commandsA.renderPasses[0].hasDepthStencil);
    EXPECT_EQ(RHILoadOp::Load,
              commandsA.renderPasses[0].depthStencilAttachment.depthLoadOp);
    EXPECT_EQ(RHIStoreOp::Store,
              commandsA.renderPasses[0].depthStencilAttachment.depthStoreOp);
    EXPECT_TRUE(commandsA.renderPasses[0].depthStencilAttachment.readOnly);
    EXPECT_FALSE(commandsB.renderPasses[0].hasDepthStencil);
}

TEST_F(RenderPassValidationFixture,
       SkyboxPassTypedPathFailsClosedForLegacyAndMalformedRecordings)
{
    RVX_REQUIRE_RENDER_RUNTIME_PIPELINE();

    SkyboxPass pass;
    pass.SetResources(&pipelineCache);
    pass.SetResourceRegistry(&gpuResources.GetRegistry());
    ASSERT_TRUE(pass.IsSupported()) << pass.GetUnsupportedReason();

    const auto makeColor = [this](uint32 extent)
    {
        return device.CreateTexture(
            RHITextureDesc::RenderTarget(extent, extent, RHIFormat::RGBA8_UNORM));
    };
    const auto makeContext = [&](RenderGraph& graph,
                                 RHITexture* color,
                                 uint64 sequence,
                                 RenderSubmissionResourceBatch* batch = nullptr)
    {
        ViewData recordView = view;
        recordView.renderGraph = &graph;
        recordView.viewCache = &viewCache;
        recordView.colorTarget = graph.ImportTexture(
            color, RHIResourceState::RenderTarget);
        RenderSkySnapshot sky;
        sky.mode = RenderSkyMode::SolidColor;
        sky.tint = Vec3{0.3f, 0.4f, 0.5f};
        return MakeSkyboxRecordContext(
            graph, recordView, scene, sky, sequence, sequence, batch);
    };
    const auto expectNoUsageOrCommands = [](RenderGraph& graph,
                                             RecordingCommandContext& commands)
    {
        graph.Compile();
        ASSERT_TRUE(graph.GetCompileStats().compileValid);
        const RenderGraph::Diagnostics diagnostics = graph.GetDiagnostics();
        ASSERT_EQ(1u, diagnostics.passes.size());
        EXPECT_TRUE(diagnostics.passes[0].usages.empty());
        graph.Execute(commands);
        EXPECT_EQ(0u, commands.beginRenderPassCount);
        EXPECT_EQ(0u, commands.drawCount);
    };

    // The ViewData overload cannot recover an execution mailbox, and the old
    // Setup/Execute entry points are inert too.
    RenderGraph legacyGraph;
    legacyGraph.SetDevice(&device);
    RHITextureRef legacyColor = makeColor(64);
    ASSERT_TRUE(legacyColor);
    ViewData legacyView = view;
    legacyView.renderGraph = &legacyGraph;
    legacyView.viewCache = &viewCache;
    legacyView.colorTarget = legacyGraph.ImportTexture(
        legacyColor.Get(), RHIResourceState::RenderTarget);
    pass.AddToGraph(legacyGraph, legacyView);
    RecordingCommandContext legacyCommands;
    expectNoUsageOrCommands(legacyGraph, legacyCommands);
    RecordingCommandContext legacyHookCommands;
    pass.Execute(legacyHookCommands, legacyView);
    EXPECT_EQ(0u, legacyHookCommands.drawCount);

    // A missing color target and a disabled pass do not publish attachments.
    RenderGraph missingColorGraph;
    missingColorGraph.SetDevice(&device);
    ViewData missingColorView = view;
    missingColorView.renderGraph = &missingColorGraph;
    missingColorView.viewCache = &viewCache;
    RenderSkySnapshot sky;
    sky.mode = RenderSkyMode::SolidColor;
    RenderPassRecordContext missingColor = MakeSkyboxRecordContext(
        missingColorGraph, missingColorView, scene, sky, 930, 930);
    pass.AddToGraph(missingColorGraph, missingColor);
    RecordingCommandContext missingColorCommands;
    expectNoUsageOrCommands(missingColorGraph, missingColorCommands);

    pass.SetEnabled(false);
    RenderGraph disabledGraph;
    disabledGraph.SetDevice(&device);
    RHITextureRef disabledColor = makeColor(64);
    ASSERT_TRUE(disabledColor);
    RenderPassRecordContext disabled = makeContext(
        disabledGraph, disabledColor.Get(), 931);
    pass.AddToGraph(disabledGraph, disabled);
    RecordingCommandContext disabledCommands;
    expectNoUsageOrCommands(disabledGraph, disabledCommands);
    pass.SetEnabled(true);

    // A disabled packet is a complete, explicit no-op even while the
    // compatibility bridge has a supported sky configured from another frame.
    RenderGraph disabledSkyGraph;
    disabledSkyGraph.SetDevice(&device);
    RHITextureRef disabledSkyColor = makeColor(64);
    ASSERT_TRUE(disabledSkyColor);
    ViewData disabledSkyView = view;
    disabledSkyView.renderGraph = &disabledSkyGraph;
    disabledSkyView.viewCache = &viewCache;
    disabledSkyView.colorTarget = disabledSkyGraph.ImportTexture(
        disabledSkyColor.Get(), RHIResourceState::RenderTarget);
    RenderSkySnapshot disabledSky;
    disabledSky.mode = RenderSkyMode::Disabled;
    RenderPassRecordContext disabledSkyContext = MakeSkyboxRecordContext(
        disabledSkyGraph, disabledSkyView, scene, disabledSky, 937, 937);
    pass.AddToGraph(disabledSkyGraph, disabledSkyContext);
    RecordingCommandContext disabledSkyCommands;
    expectNoUsageOrCommands(disabledSkyGraph, disabledSkyCommands);

    // Foreign result/snapshot pairing, stale and forged handles, and a
    // self-graph incomplete snapshot all fail before any graph declaration.
    RenderGraph sourceGraph;
    RenderGraph foreignGraph;
    sourceGraph.SetDevice(&device);
    foreignGraph.SetDevice(&device);
    RHITextureRef sourceColor = makeColor(64);
    ASSERT_TRUE(sourceColor);
    RenderPassRecordContext foreign = makeContext(
        sourceGraph, sourceColor.Get(), 932);
    foreign.results->opaqueStats.directDrawCount = 777;
    foreign.frameSnapshot.reset();
    pass.AddToGraph(foreignGraph, foreign);
    RecordingCommandContext foreignCommands;
    expectNoUsageOrCommands(foreignGraph, foreignCommands);
    EXPECT_EQ(777u, foreign.results->opaqueStats.directDrawCount);
    EXPECT_EQ(foreign.identity, foreign.results->identity);

    RenderGraph staleSourceGraph;
    RenderGraph staleGraph;
    staleSourceGraph.SetDevice(&device);
    staleGraph.SetDevice(&device);
    RHITextureRef staleColor = makeColor(64);
    ASSERT_TRUE(staleColor);
    ViewData staleView = view;
    staleView.renderGraph = &staleGraph;
    staleView.viewCache = &viewCache;
    staleView.colorTarget = staleSourceGraph.ImportTexture(
        staleColor.Get(), RHIResourceState::RenderTarget);
    RenderPassRecordContext stale = MakeSkyboxRecordContext(
        staleGraph, staleView, scene, sky, 933, 933);
    pass.AddToGraph(staleGraph, stale);
    RecordingCommandContext staleCommands;
    expectNoUsageOrCommands(staleGraph, staleCommands);

    RenderGraph forgedGraph;
    forgedGraph.SetDevice(&device);
    RGTextureHandle forgedColor;
    forgedColor.index = 0;
    forgedColor.graphIdentity = forgedGraph.GetGraphIdentity();
    forgedColor.recordingGeneration = forgedGraph.GetRecordingGeneration();
    ViewData forgedView = view;
    forgedView.renderGraph = &forgedGraph;
    forgedView.viewCache = &viewCache;
    forgedView.colorTarget = forgedColor;
    RenderPassRecordContext forged = MakeSkyboxRecordContext(
        forgedGraph, forgedView, scene, sky, 934, 934);
    pass.AddToGraph(forgedGraph, forged);
    RecordingCommandContext forgedCommands;
    expectNoUsageOrCommands(forgedGraph, forgedCommands);

    RenderGraph incompleteGraph;
    incompleteGraph.SetDevice(&device);
    RHITextureRef incompleteColor = makeColor(64);
    ASSERT_TRUE(incompleteColor);
    RenderPassRecordContext incomplete = makeContext(
        incompleteGraph, incompleteColor.Get(), 935);
    incomplete.frameSnapshot.reset();
    pass.AddToGraph(incompleteGraph, incomplete);
    RecordingCommandContext incompleteCommands;
    expectNoUsageOrCommands(incompleteGraph, incompleteCommands);

    RenderSubmissionTracker tracker;
    ASSERT_TRUE(tracker.Initialize(&device));
    RenderRetirementQueue retirement;
    ASSERT_TRUE(retirement.Initialize(&tracker));
    RenderSubmissionResourceBatch sealedBatch;
    sealedBatch.ReleaseUnsubmitted(retirement);
    ASSERT_TRUE(sealedBatch.IsSealed());
    RenderGraph sealedGraph;
    sealedGraph.SetDevice(&device);
    RHITextureRef sealedColor = makeColor(64);
    ASSERT_TRUE(sealedColor);
    RenderPassRecordContext sealed = makeContext(
        sealedGraph, sealedColor.Get(), 936, &sealedBatch);
    pass.AddToGraph(sealedGraph, sealed);
    RecordingCommandContext sealedCommands;
    expectNoUsageOrCommands(sealedGraph, sealedCommands);
    EXPECT_EQ(GPUCompletionStatus::Completed, retirement.Poll());
    tracker.Shutdown();
}

TEST_F(RenderPassValidationFixture,
       SkyboxPassSubmissionRetainsRecordingResourcesUntilCompletion)
{
    RVX_REQUIRE_RENDER_RUNTIME_PIPELINE();
    device.SetFenceAutoComplete(false);

    RenderSubmissionTracker tracker;
    ASSERT_TRUE(tracker.Initialize(&device));
    RenderRetirementQueue retirement;
    ASSERT_TRUE(retirement.Initialize(&tracker));

    SkyboxPass pass;
    pass.SetResources(&pipelineCache);
    pass.SetResourceRegistry(&gpuResources.GetRegistry());
    ASSERT_TRUE(pass.IsSupported()) << pass.GetUnsupportedReason();
    RHIPipelineLayoutRef layoutProbe(pipelineCache.GetSkyboxLayout());
    ASSERT_TRUE(layoutProbe);

    RenderGraph graph;
    graph.SetDevice(&device);
    RHITextureRef color = device.CreateTexture(
        RHITextureDesc::RenderTarget(64, 64, RHIFormat::RGBA8_UNORM));
    RHITextureRef depth = device.CreateTexture(
        RHITextureDesc::DepthStencil(
            64, 64, PipelineCache::GetDefaultDepthStencilFormat()));
    ASSERT_TRUE(color);
    ASSERT_TRUE(depth);
    ViewData recordView = view;
    recordView.renderGraph = &graph;
    recordView.viewCache = &viewCache;
    recordView.colorTarget = graph.ImportTexture(
        color.Get(), RHIResourceState::RenderTarget);
    recordView.depthTarget = graph.ImportTexture(
        depth.Get(), RHIResourceState::DepthRead);
    recordView.viewportWidth = 64;
    recordView.viewportHeight = 64;
    RenderSubmissionResourceBatch batch;
    RenderSkySnapshot sky;
    sky.mode = RenderSkyMode::SolidColor;
    sky.tint = Vec3{0.25f, 0.5f, 0.75f};
    sky.intensity = 1.25f;
    RenderPassRecordContext context = MakeSkyboxRecordContext(
        graph, recordView, scene, sky, 940, 940, &batch);
    pass.AddToGraph(graph, context);
    graph.Compile();
    ASSERT_TRUE(graph.GetCompileStats().compileValid);
    ASSERT_GT(batch.GetRetainedObjectCount(), 0u);

    const uint32 retainedBeforeExecute = batch.GetRetainedObjectCount();
    RecordingCommandContext commands;
    graph.Execute(commands);
    EXPECT_EQ(1u, commands.beginRenderPassCount);
    EXPECT_EQ(1u, commands.drawCount);
    EXPECT_GT(batch.GetRetainedObjectCount(), retainedBeforeExecute);

    GPUCompletionToken completion;
    ASSERT_TRUE(InsertGPUCompletionPoint(completion, tracker.Submit(&commands)));
    batch.SealAndTransfer(completion, retirement);
    EXPECT_TRUE(batch.IsSealed());
    EXPECT_EQ(0u, batch.GetRetainedObjectCount());
    ASSERT_GT(retirement.GetDiagnostics().entryCount, 0u);

    graph.Clear();
    pipelineCache.Shutdown();
    EXPECT_GT(layoutProbe->GetRefCount(), 1u);
    FakeFence* const completionFence =
        device.FindFenceWithSignal(completion.points[0].value);
    ASSERT_NE(completionFence, nullptr);
    completionFence->Complete(completion.points[0].value);
    EXPECT_EQ(GPUCompletionStatus::Completed, retirement.Poll());
    EXPECT_EQ(1u, layoutProbe->GetRefCount());
    tracker.Shutdown();
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

TEST_F(RenderPassValidationFixture, PostProcessSettingsDefaultToneMappingOperatorIsACES)
{
    PostProcessSettings settings;
    EXPECT_EQ(settings.toneMappingOperator, ToneMappingOperator::ACES);
}

TEST_F(RenderPassValidationFixture, ViewDataDefaultsKeepTemporalHistoryStable)
{
    ViewData defaultView;
    EXPECT_FALSE(defaultView.resetTemporalHistory);
    EXPECT_FALSE(defaultView.velocityTarget.IsValid());
    EXPECT_EQ(defaultView.previousViewProjectionValid, 0);

    RenderObject defaultObject;
    EXPECT_EQ(defaultObject.previousWorldMatrixValid, 0);
}

TEST(RenderPassStatusValidation,
     PrimaryDirectionalLightSnapshotValueCopiesAndProjectsViewData)
{
    RenderPassRecordContext context;
    context.primaryDirectionalLight.selected = true;
    context.primaryDirectionalLight.castsShadow = true;
    context.primaryDirectionalLight.direction = Vec3{0.1f, -0.7f, 0.6f};
    context.primaryDirectionalLight.color = Vec3{0.2f, 0.4f, 0.8f};
    context.primaryDirectionalLight.intensity = 3.5f;
    RenderPassRecordResults results;
    const std::shared_ptr<const RenderPassFrameSnapshot> snapshot =
        MakeRenderPassFrameSnapshot(context, results);

    ASSERT_TRUE(snapshot);
    EXPECT_TRUE(snapshot->primaryDirectionalLight.selected);
    EXPECT_TRUE(snapshot->primaryDirectionalLight.IsShadowEligible());
    EXPECT_FLOAT_EQ(snapshot->view.directionalLightDirection.x, 0.1f);
    EXPECT_FLOAT_EQ(snapshot->view.directionalLightDirection.y, -0.7f);
    EXPECT_FLOAT_EQ(snapshot->view.directionalLightDirection.z, 0.6f);
    EXPECT_FLOAT_EQ(snapshot->view.directionalLightColor.x, 0.2f);
    EXPECT_FLOAT_EQ(snapshot->view.directionalLightColor.y, 0.4f);
    EXPECT_FLOAT_EQ(snapshot->view.directionalLightColor.z, 0.8f);
    EXPECT_FLOAT_EQ(snapshot->view.directionalLightIntensity, 3.5f);

    context.primaryDirectionalLight.direction = Vec3{9.0f, 8.0f, 7.0f};
    context.primaryDirectionalLight.color = Vec3{6.0f, 5.0f, 4.0f};
    context.primaryDirectionalLight.intensity = 0.0f;
    EXPECT_FLOAT_EQ(snapshot->primaryDirectionalLight.direction.x, 0.1f);
    EXPECT_FLOAT_EQ(snapshot->primaryDirectionalLight.color.z, 0.8f);
    EXPECT_FLOAT_EQ(snapshot->primaryDirectionalLight.intensity, 3.5f);
    EXPECT_TRUE(snapshot->primaryDirectionalLight.IsShadowEligible());
}

TEST(SceneRendererPassRecordingValidation,
     PrimaryDirectionalLightSelectionPreservesSceneOrderAndCallerValues)
{
    struct SelectionCase
    {
        const char* name = "";
        std::vector<RenderLight> lights;
        bool selected = false;
        bool castsShadow = false;
        Vec3 direction{0.5f, -0.8f, 0.3f};
        Vec3 color{1.0f, 1.0f, 1.0f};
        float32 intensity = 4.0f;
        bool mutateCaller = false;
    };

    const auto directional = [](const Vec3& direction,
                                const Vec3& color,
                                float32 intensity,
                                bool castsShadow)
    {
        RenderLight light;
        light.type = RenderLight::Type::Directional;
        light.direction = direction;
        light.color = color;
        light.intensity = intensity;
        light.castsShadow = castsShadow;
        return light;
    };
    RenderLight point = directional(
        Vec3{0.0f, 0.0f, -1.0f}, Vec3{1.0f}, 10.0f, true);
    point.type = RenderLight::Type::Point;

    const std::vector<SelectionCase> cases = {
        {"no directional light", {point}, false, false},
        {"first non-caster wins over later caster",
         {directional(Vec3{0.1f, -0.9f, 0.2f}, Vec3{0.3f, 0.5f, 0.7f}, 2.0f, false),
          directional(Vec3{-0.6f, -0.4f, 0.7f}, Vec3{1.0f, 0.2f, 0.1f}, 9.0f, true)},
         true, false, Vec3{0.1f, -0.9f, 0.2f}, Vec3{0.3f, 0.5f, 0.7f}, 2.0f},
        {"first eligible caster",
         {directional(Vec3{-0.2f, -0.8f, 0.5f}, Vec3{0.8f, 0.7f, 0.6f}, 3.0f, true),
          directional(Vec3{0.4f, -0.6f, 0.7f}, Vec3{0.1f, 0.2f, 1.0f}, 4.0f, true)},
         true, true, Vec3{-0.2f, -0.8f, 0.5f}, Vec3{0.8f, 0.7f, 0.6f}, 3.0f},
        {"zero and negative intensities skip",
         {directional(Vec3{1.0f, 0.0f, 0.0f}, Vec3{1.0f, 0.0f, 0.0f}, 0.0f, true),
          directional(Vec3{0.0f, 1.0f, 0.0f}, Vec3{0.0f, 1.0f, 0.0f}, -2.0f, true),
          directional(Vec3{0.0f, 0.0f, 1.0f}, Vec3{0.0f, 0.0f, 1.0f}, 5.0f, true)},
         true, true, Vec3{0.0f, 0.0f, 1.0f}, Vec3{0.0f, 0.0f, 1.0f}, 5.0f},
        {"NaN intensity skips to the later finite directional light",
         {directional(Vec3{1.0f, 0.0f, 0.0f}, Vec3{1.0f, 0.0f, 0.0f},
                      std::numeric_limits<float32>::quiet_NaN(), true),
          directional(Vec3{0.0f, -1.0f, 0.0f}, Vec3{0.2f, 0.6f, 0.9f}, 5.0f, true)},
         true, true, Vec3{0.0f, -1.0f, 0.0f}, Vec3{0.2f, 0.6f, 0.9f}, 5.0f, true},
        {"caller mutation cannot alter copied scene light",
         {directional(Vec3{0.3f, -0.4f, 0.8f}, Vec3{0.6f, 0.4f, 0.2f}, 6.0f, true)},
         true, true, Vec3{0.3f, -0.4f, 0.8f}, Vec3{0.6f, 0.4f, 0.2f}, 6.0f, true},
    };

    for (const SelectionCase& testCase : cases)
    {
        SCOPED_TRACE(testCase.name);
        RenderScene scene;
        for (RenderLight light : testCase.lights)
        {
            scene.AddLight(light);
            if (testCase.mutateCaller)
            {
                light.direction = Vec3{9.0f, 9.0f, 9.0f};
                light.color = Vec3{8.0f, 8.0f, 8.0f};
                light.intensity = -1.0f;
                light.castsShadow = false;
            }
        }
        const PrimaryDirectionalLightRecordInput& selected =
            SelectPrimaryDirectionalLightRecordInput(scene);
        EXPECT_EQ(selected.selected, testCase.selected);
        EXPECT_EQ(selected.castsShadow, testCase.castsShadow);
        EXPECT_FLOAT_EQ(selected.direction.x, testCase.direction.x);
        EXPECT_FLOAT_EQ(selected.direction.y, testCase.direction.y);
        EXPECT_FLOAT_EQ(selected.direction.z, testCase.direction.z);
        EXPECT_FLOAT_EQ(selected.color.x, testCase.color.x);
        EXPECT_FLOAT_EQ(selected.color.y, testCase.color.y);
        EXPECT_FLOAT_EQ(selected.color.z, testCase.color.z);
        EXPECT_FLOAT_EQ(selected.intensity, testCase.intensity);
        EXPECT_EQ(selected.IsShadowEligible(), testCase.selected && testCase.castsShadow);

        RenderPassRecordContext context;
        context.primaryDirectionalLight = selected;
        RenderPassRecordResults results;
        const std::shared_ptr<const RenderPassFrameSnapshot> snapshot =
            MakeRenderPassFrameSnapshot(context, results);
        ASSERT_TRUE(snapshot);
        EXPECT_TRUE(std::isfinite(snapshot->view.directionalLightDirection.x));
        EXPECT_TRUE(std::isfinite(snapshot->view.directionalLightDirection.y));
        EXPECT_TRUE(std::isfinite(snapshot->view.directionalLightDirection.z));
        EXPECT_TRUE(std::isfinite(snapshot->view.directionalLightColor.x));
        EXPECT_TRUE(std::isfinite(snapshot->view.directionalLightColor.y));
        EXPECT_TRUE(std::isfinite(snapshot->view.directionalLightColor.z));
        EXPECT_TRUE(std::isfinite(snapshot->view.directionalLightIntensity));
    }
}

TEST(SceneRendererExternalTargetValidation, ImportsExternalColorAndDepthTargets)
{
    FakeDevice device;
    SceneRenderer renderer;
    auto graph = std::make_unique<RenderGraph>();
    graph->SetDevice(&device);
    renderer.SetRenderGraphForTesting(std::move(graph));

    RHITextureDesc colorDesc = RHITextureDesc::RenderTarget(96, 64, RHIFormat::RGBA8_UNORM);
    colorDesc.usage = RHITextureUsage::RenderTarget | RHITextureUsage::ShaderResource;
    RHITextureRef colorTarget = device.CreateTexture(colorDesc);
    ASSERT_TRUE(colorTarget);

    RHITextureDesc depthDesc = RHITextureDesc::DepthStencil(96, 64, PipelineCache::GetDefaultDepthStencilFormat());
    RHITextureRef depthTarget = device.CreateTexture(depthDesc);
    ASSERT_TRUE(depthTarget);

    SceneRendererExternalTargetDesc externalTarget;
    externalTarget.colorTarget = colorTarget.Get();
    externalTarget.depthTarget = depthTarget.Get();
    externalTarget.colorInitialState = RHIResourceState::ShaderResource;
    externalTarget.colorFinalState = RHIResourceState::ShaderResource;
    externalTarget.depthInitialState = RHIResourceState::DepthRead;
    externalTarget.depthFinalState = RHIResourceState::DepthRead;

    renderer.SetExternalRenderTarget(externalTarget);
    renderer.BuildRenderGraphForTesting();

    const SceneRendererExternalTargetStats& stats = renderer.GetExternalRenderTargetStats();
    EXPECT_TRUE(stats.requested);
    EXPECT_TRUE(stats.active);
    EXPECT_TRUE(stats.importedColor);
    EXPECT_TRUE(stats.importedDepth);
    EXPECT_EQ(stats.width, 96u);
    EXPECT_EQ(stats.height, 64u);
    EXPECT_EQ(stats.colorFormat, RHIFormat::RGBA8_UNORM);
    EXPECT_EQ(stats.depthFormat, PipelineCache::GetDefaultDepthStencilFormat());
    EXPECT_EQ(stats.colorFinalState, RHIResourceState::ShaderResource);
    EXPECT_EQ(stats.depthFinalState, RHIResourceState::DepthRead);
    EXPECT_TRUE(stats.fallbackReason.empty());

    EXPECT_EQ(renderer.GetViewData().viewportWidth, 96u);
    EXPECT_EQ(renderer.GetViewData().viewportHeight, 64u);
    EXPECT_EQ(renderer.GetRenderGraph()->GetTexture(renderer.GetViewData().colorTarget), colorTarget.Get());
    EXPECT_EQ(renderer.GetRenderGraph()->GetTexture(renderer.GetViewData().depthTarget), depthTarget.Get());
    EXPECT_EQ(renderer.GetPostProcessStats().backBufferFormat, RHIFormat::RGBA8_UNORM);
    EXPECT_EQ(renderer.GetPostProcessStats().toneMappingOutputColorSpace, ToneMappingOutputColorSpace::SRGB);
    EXPECT_EQ(renderer.GetFrameDiagnostics().toneMappingOutputColorSpace, ToneMappingOutputColorSpace::SRGB);
}

TEST(SceneRendererPassRecordingValidation,
     RegistryBuildUsesTypedRecordContextInsteadOfViewDataOverload)
{
    FakeDevice device;
    SceneRenderer renderer;
    auto graph = std::make_unique<RenderGraph>();
    graph->SetDevice(&device);
    renderer.SetRenderGraphForTesting(std::move(graph));

    RHITextureDesc colorDesc = RHITextureDesc::RenderTarget(
        96, 64, RHIFormat::RGBA8_UNORM);
    colorDesc.usage = RHITextureUsage::RenderTarget |
        RHITextureUsage::ShaderResource;
    RHITextureRef colorTarget = device.CreateTexture(colorDesc);
    ASSERT_TRUE(colorTarget);

    RHITextureDesc depthDesc = RHITextureDesc::DepthStencil(
        96, 64, PipelineCache::GetDefaultDepthStencilFormat());
    RHITextureRef depthTarget = device.CreateTexture(depthDesc);
    ASSERT_TRUE(depthTarget);

    SceneRendererExternalTargetDesc externalTarget;
    externalTarget.colorTarget = colorTarget.Get();
    externalTarget.depthTarget = depthTarget.Get();
    externalTarget.colorInitialState = RHIResourceState::ShaderResource;
    externalTarget.colorFinalState = RHIResourceState::ShaderResource;
    externalTarget.depthInitialState = RHIResourceState::DepthRead;
    externalTarget.depthFinalState = RHIResourceState::DepthRead;
    renderer.SetExternalRenderTarget(externalTarget);

    auto probe = std::make_unique<SceneRendererOverloadProbePass>();
    SceneRendererOverloadProbePass* const probePtr = probe.get();
    ASSERT_NE(nullptr, probePtr);
    renderer.AddPass(std::move(probe));
    EXPECT_EQ(1u, renderer.GetPassCount());

    // AddPass invalidates the renderer's working plan, so establish the
    // frame identity and mutable caller values after registration.
    RenderFrameExecutionPlan plan;
    plan.frameSequence = 7001;
    plan.viewOrdinal = 3;
    renderer.GetViewData().renderFrameExecutionPlan = &plan;
    renderer.GetViewData().viewportWidth = 401;
    renderer.GetViewData().viewportHeight = 233;
    renderer.GetViewData().frameNumber = 73;

    renderer.BuildRenderGraphForTesting();

    EXPECT_EQ(0u, probePtr->viewDataOverloadCount);
    EXPECT_EQ(1u, probePtr->typedOverloadCount);
    EXPECT_FALSE(probePtr->typedLegacyAdapter);
    EXPECT_TRUE(probePtr->typedIdentity.Matches(*renderer.GetRenderGraph()));
    EXPECT_TRUE(probePtr->typedContextIdentityValid);
    EXPECT_TRUE(probePtr->typedResourcesBelongToGraph);
    EXPECT_TRUE(probePtr->typedExecutionValid);
    EXPECT_EQ(probePtr->typedColor.index, probePtr->typedSnapshotColor.index);
    EXPECT_EQ(probePtr->typedColor.graphIdentity,
              probePtr->typedSnapshotColor.graphIdentity);
    EXPECT_EQ(probePtr->typedColor.recordingGeneration,
              probePtr->typedSnapshotColor.recordingGeneration);
    EXPECT_EQ(probePtr->typedDepth.index, probePtr->typedSnapshotDepth.index);
    EXPECT_EQ(probePtr->typedDepth.graphIdentity,
              probePtr->typedSnapshotDepth.graphIdentity);
    EXPECT_EQ(probePtr->typedDepth.recordingGeneration,
              probePtr->typedSnapshotDepth.recordingGeneration);
    EXPECT_EQ(colorTarget.Get(),
              renderer.GetRenderGraph()->GetTexture(probePtr->typedColor));
    EXPECT_EQ(depthTarget.Get(),
              renderer.GetRenderGraph()->GetTexture(probePtr->typedDepth));

    // The production adapter must retain its value-owned snapshot rather than
    // observe post-build mutations to SceneRenderer's working ViewData/plan.
    renderer.GetViewData().viewportWidth = 999;
    renderer.GetViewData().frameNumber = 88;
    plan.frameSequence = 7999;

    renderer.GetRenderGraph()->Compile();
    ASSERT_TRUE(renderer.GetRenderGraph()->GetCompileStats().compileValid);
    RecordingCommandContext commands;
    renderer.GetRenderGraph()->Execute(commands);

    EXPECT_EQ(1u, probePtr->setupCount);
    EXPECT_EQ(probePtr->typedColor.index, probePtr->setupColor.index);
    EXPECT_EQ(probePtr->typedDepth.index, probePtr->setupDepth.index);
    EXPECT_EQ(401u, probePtr->setupViewportWidth);
    EXPECT_EQ(73u, probePtr->setupFrameNumber);
    EXPECT_EQ(1u, probePtr->recordedExecutionCount);
    EXPECT_EQ(probePtr->typedColor.index, probePtr->recordedColor.index);
    EXPECT_EQ(probePtr->typedDepth.index, probePtr->recordedDepth.index);
    EXPECT_EQ(401u, probePtr->recordedViewportWidth);
    EXPECT_EQ(73u, probePtr->recordedFrameNumber);
    EXPECT_EQ(7001u, probePtr->recordedFrameSequence);
}

TEST(SceneRendererExternalTargetValidation, InvalidExternalColorTargetFallsBackWithoutImport)
{
    FakeDevice device;
    SceneRenderer renderer;
    auto graph = std::make_unique<RenderGraph>();
    graph->SetDevice(&device);
    renderer.SetRenderGraphForTesting(std::move(graph));

    RHITextureDesc invalidColorDesc = RHITextureDesc::RenderTarget(0, 64, RHIFormat::RGBA8_UNORM);
    RHITextureRef invalidColorTarget = device.CreateTexture(invalidColorDesc);
    ASSERT_TRUE(invalidColorTarget);

    SceneRendererExternalTargetDesc externalTarget;
    externalTarget.colorTarget = invalidColorTarget.Get();
    renderer.SetExternalRenderTarget(externalTarget);
    renderer.BuildRenderGraphForTesting();

    const SceneRendererExternalTargetStats& stats = renderer.GetExternalRenderTargetStats();
    EXPECT_TRUE(stats.requested);
    EXPECT_FALSE(stats.active);
    EXPECT_FALSE(stats.importedColor);
    EXPECT_FALSE(stats.importedDepth);
    EXPECT_NE(stats.fallbackReason.find("External color target"), std::string::npos);
    EXPECT_FALSE(renderer.GetViewData().colorTarget.IsValid());
}

TEST(SceneRendererExternalTargetValidation, FloatExternalTargetKeepsToneMappingOutputLinear)
{
    FakeDevice device;
    SceneRenderer renderer;
    auto graph = std::make_unique<RenderGraph>();
    graph->SetDevice(&device);
    renderer.SetRenderGraphForTesting(std::move(graph));

    RHITextureDesc colorDesc = RHITextureDesc::RenderTarget(64, 32, RHIFormat::RGBA16_FLOAT);
    RHITextureRef colorTarget = device.CreateTexture(colorDesc);
    ASSERT_TRUE(colorTarget);

    SceneRendererExternalTargetDesc externalTarget;
    externalTarget.colorTarget = colorTarget.Get();
    externalTarget.colorInitialState = RHIResourceState::ShaderResource;
    externalTarget.colorFinalState = RHIResourceState::ShaderResource;

    renderer.SetExternalRenderTarget(externalTarget);
    renderer.BuildRenderGraphForTesting();

    const SceneRenderPostProcessStats& stats = renderer.GetPostProcessStats();
    EXPECT_EQ(stats.backBufferFormat, RHIFormat::RGBA16_FLOAT);
    EXPECT_EQ(stats.toneMappingOutputFormat, RHIFormat::RGBA16_FLOAT);
    EXPECT_EQ(stats.toneMappingOutputColorSpace, ToneMappingOutputColorSpace::Linear);
    EXPECT_EQ(renderer.GetFrameDiagnostics().toneMappingOutputColorSpace, ToneMappingOutputColorSpace::Linear);
}

TEST(SceneRendererDiagnosticsValidation, AggregatesExternalTargetPassChainAndRenderGraphStats)
{
    FakeDevice device;
    SceneRenderer renderer;
    auto graph = std::make_unique<RenderGraph>();
    graph->SetDevice(&device);
    renderer.SetRenderGraphForTesting(std::move(graph));

    RHITextureDesc colorDesc = RHITextureDesc::RenderTarget(96, 64, RHIFormat::RGBA8_UNORM);
    colorDesc.usage = RHITextureUsage::RenderTarget | RHITextureUsage::ShaderResource;
    RHITextureRef colorTarget = device.CreateTexture(colorDesc);
    ASSERT_TRUE(colorTarget);

    RHITextureDesc depthDesc = RHITextureDesc::DepthStencil(96, 64, PipelineCache::GetDefaultDepthStencilFormat());
    RHITextureRef depthTarget = device.CreateTexture(depthDesc);
    ASSERT_TRUE(depthTarget);

    SceneRendererExternalTargetDesc externalTarget;
    externalTarget.colorTarget = colorTarget.Get();
    externalTarget.depthTarget = depthTarget.Get();
    externalTarget.colorInitialState = RHIResourceState::ShaderResource;
    externalTarget.colorFinalState = RHIResourceState::ShaderResource;
    externalTarget.depthInitialState = RHIResourceState::DepthRead;
    externalTarget.depthFinalState = RHIResourceState::DepthRead;
    renderer.SetExternalRenderTarget(externalTarget);

    RenderObject object;
    object.entityId = 42;
    renderer.GetRenderScene().AddObject(object);

    RenderLight light;
    light.type = RenderLight::Type::Directional;
    renderer.GetRenderScene().AddLight(light);

    renderer.BuildRenderGraphForTesting();

    const SceneRendererFrameDiagnostics& diagnostics = renderer.GetFrameDiagnostics();
    EXPECT_FALSE(diagnostics.renderAttempted);
    EXPECT_FALSE(diagnostics.rendered);
    EXPECT_TRUE(diagnostics.graphBuilt);
    EXPECT_FALSE(diagnostics.graphCompiled);
    EXPECT_TRUE(diagnostics.graphCompileValid);
    EXPECT_TRUE(diagnostics.skippedReason.empty());

    EXPECT_TRUE(diagnostics.externalTargetRequested);
    EXPECT_TRUE(diagnostics.externalTargetActive);
    EXPECT_TRUE(diagnostics.externalColorImported);
    EXPECT_TRUE(diagnostics.externalDepthImported);
    EXPECT_TRUE(diagnostics.externalTargetFallbackReason.empty());

    EXPECT_EQ(diagnostics.renderSceneObjectCount, static_cast<size_t>(1));
    EXPECT_EQ(diagnostics.renderSceneLightCount, static_cast<size_t>(1));
    EXPECT_EQ(diagnostics.registeredPassCount, static_cast<size_t>(0));
    EXPECT_EQ(diagnostics.graphPassCount, static_cast<size_t>(0));
    EXPECT_EQ(diagnostics.skippedDisabledPassCount, static_cast<size_t>(0));
    EXPECT_EQ(diagnostics.skippedUnsupportedPassCount, static_cast<size_t>(0));
    EXPECT_TRUE(diagnostics.passStatuses.empty());
    EXPECT_TRUE(diagnostics.graphDiagnostics.empty());
}

TEST(SceneRendererDiagnosticsValidation,
     RenderPolicyPlanUsesFrameLifetimeAndInvalidatesAtOwnershipBoundaries)
{
    SceneRenderer renderer;
    renderer.SetGPUDrivenCullingMode(RenderGPUDrivenMode::ForceDisabled);
    EXPECT_EQ(renderer.GetGPUDrivenCullingMode(),
              renderer.GetGPUDrivenPolicyDecision().requestedMode);
    renderer.CompileRenderFramePlanForTesting();
    ASSERT_TRUE(renderer.GetRenderPolicyDiagnostics().requestAvailable);
    ASSERT_TRUE(renderer.GetRenderPolicyDiagnostics().planAvailable);
    const RenderPolicyMeasurement initialMeasurement =
        renderer.GetRenderPolicyDiagnostics().measurement;
    EXPECT_TRUE(initialMeasurement.planCpuTimingAvailable);
    EXPECT_FALSE(initialMeasurement.submissionCpuTimingAvailable);
    EXPECT_EQ(renderer.GetRenderPolicyDiagnostics().selectedPlan.frameSequence,
              initialMeasurement.frameSequence);
    EXPECT_EQ(0u, initialMeasurement.candidatePacketCount);
    EXPECT_EQ(0u, initialMeasurement.drawGroupCount);
    EXPECT_FALSE(initialMeasurement.averageGroupOccupancyAvailable);
    EXPECT_TRUE(initialMeasurement.nonGating);
    EXPECT_FALSE(initialMeasurement.usedForAutoDecision);
    const RenderFrameExecutionPlan ownedPlan =
        renderer.GetRenderPolicyDiagnostics().selectedPlan;
    EXPECT_EQ(0u, ownedPlan.viewOrdinal);
    EXPECT_EQ(RenderGPUDrivenMode::ForceDisabled,
              ownedPlan.viewPolicy.requestedMode);

    renderer.PrepareForSwapChainResize();
    EXPECT_FALSE(renderer.GetRenderPolicyDiagnostics().planAvailable);

    renderer.CompileRenderFramePlanForTesting();
    ASSERT_TRUE(renderer.GetRenderPolicyDiagnostics().planAvailable);
    renderer.ClearExternalRenderTarget();
    EXPECT_FALSE(renderer.GetRenderPolicyDiagnostics().planAvailable);

    renderer.CompileRenderFramePlanForTesting();
    ASSERT_TRUE(renderer.GetRenderPolicyDiagnostics().planAvailable);
    renderer.SetGPUDrivenCullingConfig(GPUCullingConfig{});
    EXPECT_FALSE(renderer.GetRenderPolicyDiagnostics().planAvailable);

    renderer.CompileRenderFramePlanForTesting();
    renderer.RefreshFrameDiagnosticsForTesting();
    EXPECT_TRUE(renderer.GetFrameDiagnostics().policy.planAvailable);
    renderer.Shutdown();
    EXPECT_FALSE(renderer.GetRenderPolicyDiagnostics().planAvailable);
    EXPECT_FALSE(renderer.GetFrameDiagnostics().policy.planAvailable);
    EXPECT_EQ(RenderGPUDrivenMode::ForceDisabled,
              ownedPlan.viewPolicy.requestedMode);
}

TEST_F(RenderPassValidationFixture, PostProcessSettingsDefaultRayTracedReflectionControlsAreExplicit)
{
    PostProcessSettings settings;

    EXPECT_FALSE(settings.enableSSR);
    EXPECT_FALSE(settings.enableRayTracedReflections);
    EXPECT_TRUE(settings.enableRayTracedReflectionDenoise);
    EXPECT_FLOAT_EQ(settings.rayTracedReflectionIntensity, 1.0f);
    EXPECT_FLOAT_EQ(settings.rayTracedReflectionResolutionScale, 1.0f);
    EXPECT_FLOAT_EQ(settings.rayTracedReflectionMaxDistance, 50.0f);
    EXPECT_FLOAT_EQ(settings.rayTracedReflectionMaxRoughness, 1.0f);
    EXPECT_FLOAT_EQ(settings.rayTracedReflectionDistanceFadeStart, 0.8f);
    EXPECT_EQ(settings.rayTracedReflectionInstanceMask, 0xFFu);
    EXPECT_EQ(settings.rayTracedReflectionSamplesPerPixel, 1u);
    EXPECT_FLOAT_EQ(settings.rayTracedReflectionRoughnessConeSpread, 1.0f);
    EXPECT_FLOAT_EQ(settings.rayTracedReflectionNormalBias, 0.02f);
    EXPECT_FLOAT_EQ(settings.rayTracedReflectionRayMinT, 0.001f);
    EXPECT_FLOAT_EQ(settings.rayTracedReflectionFireflyClamp, 64.0f);
    EXPECT_FLOAT_EQ(settings.rayTracedReflectionTemporalBlendFactor, 0.85f);
    EXPECT_FLOAT_EQ(settings.rayTracedReflectionHistoryDepthThreshold, 0.01f);
    EXPECT_FLOAT_EQ(settings.rayTracedReflectionHistoryNormalThreshold, 0.85f);
    EXPECT_FLOAT_EQ(settings.rayTracedReflectionHistoryLuminanceTolerance, 4.0f);
    EXPECT_FLOAT_EQ(settings.rayTracedReflectionHistoryConfidenceThreshold, 0.05f);
    EXPECT_FLOAT_EQ(settings.rayTracedReflectionHistoryVelocityRejectionScale, 8.0f);
    EXPECT_EQ(settings.rayTracedReflectionDenoiseRadius, 1u);
    EXPECT_FLOAT_EQ(settings.rayTracedReflectionDenoiseDepthSigma, 0.01f);
    EXPECT_FLOAT_EQ(settings.rayTracedReflectionDenoiseNormalThreshold, 0.85f);
    EXPECT_FLOAT_EQ(settings.rayTracedReflectionDenoiseConfidencePower, 1.0f);
    EXPECT_FLOAT_EQ(settings.rayTracedReflectionDenoiseCenterWeight, 1.0f);
    EXPECT_FLOAT_EQ(settings.rayTracedReflectionDenoiseLowConfidenceDepthScale, 4.0f);

    SceneRayTracingBudgetSettings budgetSettings;
    EXPECT_FALSE(budgetSettings.enabled);
    EXPECT_EQ(budgetSettings.maxRayCount, 0u);
    EXPECT_EQ(budgetSettings.maxDenoiseTapCount, 0u);
    EXPECT_EQ(budgetSettings.maxTrackedResourceBytes, 0u);
    EXPECT_FLOAT_EQ(budgetSettings.maxMeasuredGpuMs, 0.0f);
    EXPECT_FLOAT_EQ(budgetSettings.maxShadowMeasuredGpuMs, 0.0f);
    EXPECT_FLOAT_EQ(budgetSettings.maxReflectionMeasuredGpuMs, 0.0f);
    EXPECT_FLOAT_EQ(budgetSettings.gpuTimingHysteresis, 0.15f);
    EXPECT_FLOAT_EQ(budgetSettings.gpuTimingRecoveryRate, 0.05f);
    EXPECT_EQ(budgetSettings.gpuTimingAdjustmentFrameCount, 2u);
    EXPECT_FLOAT_EQ(budgetSettings.minReflectionResolutionScale, 0.25f);
    EXPECT_EQ(budgetSettings.minShadowSamplesPerPixel, 1u);
    EXPECT_EQ(budgetSettings.minReflectionSamplesPerPixel, 1u);
    EXPECT_EQ(budgetSettings.minReflectionDenoiseRadius, 0u);
    EXPECT_EQ(budgetSettings.blasCacheEvictionFrameThreshold, 300u);

    SceneRayTracingFrameStats frameStats;
    EXPECT_FALSE(frameStats.resourceBudgetExceeded);
    EXPECT_FALSE(frameStats.resourceBudgetEvictionAttempted);
    EXPECT_FALSE(frameStats.resourceByteAccountingOverflowed);
    EXPECT_EQ(frameStats.trackedResourceBudget, 0u);
    EXPECT_EQ(frameStats.resourceBudgetEvictedBLASCount, 0u);
    EXPECT_EQ(frameStats.releasedBLASScratchCount, 0u);
    EXPECT_EQ(frameStats.pendingBLASScratchReleaseCount, 0u);
    EXPECT_EQ(frameStats.cachedBLASAccelerationStructureBytes, 0u);
    EXPECT_EQ(frameStats.cachedBLASScratchBytes, 0u);
    EXPECT_EQ(frameStats.releasedBLASScratchBytes, 0u);
    EXPECT_EQ(frameStats.topLevelAccelerationStructureBytes, 0u);
    EXPECT_EQ(frameStats.topLevelScratchBytes, 0u);
    EXPECT_EQ(frameStats.instanceBufferBytes, 0u);
    EXPECT_EQ(frameStats.materialMetadataBufferBytes, 0u);
    EXPECT_EQ(frameStats.alphaMetadataBufferBytes, 0u);
    EXPECT_EQ(frameStats.totalTrackedResourceBytes, 0u);
}

TEST_F(RenderPassValidationFixture, ToneMappingConfigureAppliesOperatorFromSettings)
{
    ToneMappingPass pass;
    PostProcessSettings settings;
    settings.enableToneMapping = true;
    settings.toneMappingOperator = ToneMappingOperator::Neutral;

    pass.Configure(settings);
    EXPECT_EQ(pass.GetOperator(), ToneMappingOperator::Neutral);

    settings.toneMappingOperator = ToneMappingOperator::None;
    pass.Configure(settings);
    EXPECT_EQ(pass.GetOperator(), ToneMappingOperator::None);
}

TEST_F(RenderPassValidationFixture, ToneMappingConfigureAppliesOutputColorSpaceFromSettings)
{
    ToneMappingPass pass;
    PostProcessSettings settings;
    settings.enableToneMapping = true;
    settings.toneMappingOutputColorSpace = ToneMappingOutputColorSpace::Linear;

    pass.Configure(settings);
    EXPECT_EQ(pass.GetOutputColorSpace(), ToneMappingOutputColorSpace::Linear);

    settings.toneMappingOutputColorSpace = ToneMappingOutputColorSpace::SRGB;
    pass.Configure(settings);
    EXPECT_EQ(pass.GetOutputColorSpace(), ToneMappingOutputColorSpace::SRGB);
}

TEST_F(RenderPassValidationFixture, ToneMappingConfigureResolvesManualExposureSettings)
{
    ToneMappingPass pass;
    PostProcessSettings settings;
    pass.Configure(settings);
    EXPECT_FLOAT_EQ(pass.GetExposure(), 1.0f);

    settings.exposure = 1.25f;
    pass.Configure(settings);
    EXPECT_FLOAT_EQ(pass.GetExposure(), 1.25f);
}

TEST_F(RenderPassValidationFixture, ToneMappingConfigureSanitizesManualExposureSettings)
{
    ToneMappingPass pass;
    PostProcessSettings settings;

    settings.exposure = std::numeric_limits<float>::quiet_NaN();
    pass.Configure(settings);
    EXPECT_FLOAT_EQ(pass.GetExposure(), 1.0f);

    settings.exposure = std::numeric_limits<float>::infinity();
    pass.Configure(settings);
    EXPECT_FLOAT_EQ(pass.GetExposure(), 1.0f);

    settings.exposure = -1.0f;
    pass.Configure(settings);
    EXPECT_FLOAT_EQ(pass.GetExposure(), 0.0f);

    settings.exposure = 70000.0f;
    pass.Configure(settings);
    EXPECT_FLOAT_EQ(pass.GetExposure(), 65536.0f);
}

TEST_F(RenderPassValidationFixture, ToneMappingConfigureResolvesCameraEV100ExposureSettings)
{
    ToneMappingPass pass;
    PostProcessSettings settings;
    settings.exposureMode = ToneMappingExposureMode::CameraEV100;
    settings.cameraEV100 = 2.0f;
    settings.exposureCompensationEV = 1.0f;

    pass.Configure(settings);
    EXPECT_NEAR(pass.GetExposure(), 0.5f, 0.00001f);

    settings.cameraEV100 = -20.0f;
    settings.exposureCompensationEV = 20.0f;
    pass.Configure(settings);
    EXPECT_NEAR(pass.GetExposure(), 65536.0f, 0.01f);
}

TEST_F(RenderPassValidationFixture, ToneMappingConfigureFallsBackForInvalidCameraEV100ExposureSettings)
{
    ToneMappingPass pass;
    PostProcessSettings settings;
    settings.exposureMode = ToneMappingExposureMode::CameraEV100;

    settings.cameraEV100 = std::numeric_limits<float>::quiet_NaN();
    settings.exposureCompensationEV = 0.0f;
    pass.Configure(settings);
    EXPECT_FLOAT_EQ(pass.GetExposure(), 1.0f);

    settings.cameraEV100 = 0.0f;
    settings.exposureCompensationEV = std::numeric_limits<float>::infinity();
    pass.Configure(settings);
    EXPECT_FLOAT_EQ(pass.GetExposure(), 1.0f);
}

TEST_F(RenderPassValidationFixture, ToneMappingAddsLiveGraphPassAndDrawsFullscreenTriangle)
{
    RVX_REQUIRE_RENDER_RUNTIME_PIPELINE();

    ToneMappingPass pass;
    PostProcessSettings settings;
    settings.enableToneMapping = true;
    settings.exposure = 1.25f;
    settings.gamma = 2.2f;
    settings.toneMappingOutputColorSpace = ToneMappingOutputColorSpace::Linear;
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
    ASSERT_EQ(descriptorIt->bindings.size(), static_cast<size_t>(4));

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
    EXPECT_TRUE(hasBinding(3, false, true, false));
    EXPECT_EQ(descriptorIt->bindings[3].textureView, descriptorIt->bindings[1].textureView);

    const FakeBuffer* constants = FindCreatedBuffer(device, "ToneMappingConstants");
    ASSERT_NE(constants, nullptr);
    const std::vector<uint8>& storage = constants->GetStorage();
    ASSERT_GE(storage.size(), static_cast<size_t>(36));

    auto readUInt = [&storage](size_t offset)
    {
        uint32 value = 0;
        std::memcpy(&value, storage.data() + offset, sizeof(value));
        return value;
    };

    EXPECT_EQ(readUInt(32), static_cast<uint32>(ToneMappingOutputColorSpace::Linear));

    auto descriptorCall = std::find(ctx.callSequence.begin(), ctx.callSequence.end(), "SetDescriptorSet");
    auto drawCall = std::find(ctx.callSequence.begin(), ctx.callSequence.end(), "Draw");
    ASSERT_NE(descriptorCall, ctx.callSequence.end());
    ASSERT_NE(drawCall, ctx.callSequence.end());
    EXPECT_LT(std::distance(ctx.callSequence.begin(), descriptorCall),
              std::distance(ctx.callSequence.begin(), drawCall));
}

TEST_F(RenderPassValidationFixture, ToneMappingSkipsDrawWhenConstantsCannotMap)
{
    RVX_REQUIRE_RENDER_RUNTIME_PIPELINE(false);

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
    RVX_REQUIRE_RENDER_RUNTIME_PIPELINE();

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
    EXPECT_EQ(stats.totalPasses, 7u);
    EXPECT_EQ(stats.culledPasses, 0u);
    EXPECT_EQ(stats.emptyPassUsageCount, 0u);

    const RenderGraph::Diagnostics diagnostics = graph.GetDiagnostics();
    for (const char* additivePassName :
         {"BloomComposite2", "BloomComposite1", "BloomComposite0"})
    {
        const auto additivePass = std::find_if(
            diagnostics.passes.begin(),
            diagnostics.passes.end(),
            [additivePassName](const RenderGraph::PassDiagnostic& passDiagnostic)
            {
                return passDiagnostic.name == additivePassName;
            });
        ASSERT_NE(additivePass, diagnostics.passes.end());
        const auto outputUsage = std::find_if(
            additivePass->usages.begin(),
            additivePass->usages.end(),
            [output](const RenderGraph::ResourceUsageDiagnostic& usage)
            {
                return usage.type == RenderGraph::DiagnosticResourceType::Texture &&
                       usage.resourceIndex == output.index;
            });
        ASSERT_NE(outputUsage, additivePass->usages.end());
        EXPECT_EQ(
            outputUsage->access,
            RenderGraph::DiagnosticAccessType::ReadWrite);
        EXPECT_EQ(outputUsage->desiredState, RHIResourceState::RenderTarget);
        EXPECT_FALSE(HasAnyAccess(
            outputUsage->desiredAccess.memoryAccess,
            RHIMemoryAccess::ShaderRead));
        EXPECT_TRUE(HasAnyAccess(
            outputUsage->desiredAccess.memoryAccess,
            RHIMemoryAccess::ColorRead));
        EXPECT_TRUE(HasAnyAccess(
            outputUsage->desiredAccess.memoryAccess,
            RHIMemoryAccess::ColorWrite));
    }

    RecordingCommandContext ctx;
    graph.Execute(ctx);

    EXPECT_EQ(ctx.beginRenderPassCount, 7u);
    EXPECT_EQ(ctx.endRenderPassCount, 7u);
    ASSERT_EQ(ctx.pipelineSequence.size(), static_cast<size_t>(7));
    for (size_t i = 0; i < 4; ++i)
    {
        ExpectPipelineDebugName(ctx.pipelineSequence[i], "BloomPipeline");
    }
    for (size_t i = 4; i < 7; ++i)
    {
        ExpectPipelineDebugName(ctx.pipelineSequence[i], "BloomAdditivePipeline");
    }
    ASSERT_EQ(ctx.descriptorSetSequence.size(), static_cast<size_t>(7));
    EXPECT_TRUE(std::all_of(ctx.descriptorSetSequence.begin(),
                            ctx.descriptorSetSequence.end(),
                            [](uint32 set) { return set == 0u; }));
    EXPECT_EQ(ctx.drawCount, 7u);
    EXPECT_EQ(ctx.lastDrawVertexCount, 3u);
    EXPECT_EQ(ctx.drawIndexedCount, 0u);

    const size_t passConstantCount = static_cast<size_t>(std::count_if(
        device.createdBufferDescs.begin(),
        device.createdBufferDescs.end(),
        [](const RHIBufferDesc& desc)
        {
            return desc.debugName &&
                   std::string(desc.debugName) == "BloomPassConstants";
        }));
    EXPECT_EQ(passConstantCount, static_cast<size_t>(7));

    std::vector<RHITextureDesc> pyramidDescs;
    for (const RHITextureDesc& desc : device.createdTextureDescs)
    {
        if (desc.debugName && std::string(desc.debugName) == "BloomPyramidLevel")
        {
            pyramidDescs.push_back(desc);
        }
    }
    ASSERT_EQ(pyramidDescs.size(), static_cast<size_t>(3));
    const uint32 expectedPyramidWidths[] = {32u, 16u, 8u};
    const uint32 expectedPyramidHeights[] = {32u, 16u, 8u};
    for (size_t i = 0; i < pyramidDescs.size(); ++i)
    {
        EXPECT_EQ(pyramidDescs[i].format, RHIFormat::RGBA16_FLOAT);
        EXPECT_EQ(pyramidDescs[i].width, expectedPyramidWidths[i]);
        EXPECT_EQ(pyramidDescs[i].height, expectedPyramidHeights[i]);
    }

    ASSERT_EQ(ctx.renderPasses.size(), static_cast<size_t>(7));
    const uint32 expectedWidths[] = {32u, 64u, 16u, 8u, 64u, 64u, 64u};
    const uint32 expectedHeights[] = {32u, 64u, 16u, 8u, 64u, 64u, 64u};
    for (size_t i = 0; i < ctx.renderPasses.size(); ++i)
    {
        EXPECT_EQ(ctx.renderPasses[i].colorAttachmentCount, 1u);
        EXPECT_FALSE(ctx.renderPasses[i].hasDepthStencil);
        EXPECT_EQ(ctx.renderPasses[i].renderArea.width, expectedWidths[i]);
        EXPECT_EQ(ctx.renderPasses[i].renderArea.height, expectedHeights[i]);
        EXPECT_EQ(ctx.renderPasses[i].colorAttachments[0].loadOp,
                  i >= 4 ? RHILoadOp::Load : RHILoadOp::DontCare);
    }

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
    ASSERT_EQ(descriptorIt->bindings.size(), static_cast<size_t>(4));

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
    EXPECT_TRUE(hasBinding(3, false, true, false));
    EXPECT_EQ(descriptorIt->bindings[3].textureView, descriptorIt->bindings[1].textureView);

    auto descriptorCall = std::find(ctx.callSequence.begin(), ctx.callSequence.end(), "SetDescriptorSet");
    auto drawCall = std::find(ctx.callSequence.begin(), ctx.callSequence.end(), "Draw");
    ASSERT_NE(descriptorCall, ctx.callSequence.end());
    ASSERT_NE(drawCall, ctx.callSequence.end());
    EXPECT_LT(std::distance(ctx.callSequence.begin(), descriptorCall),
              std::distance(ctx.callSequence.begin(), drawCall));
}

TEST_F(RenderPassValidationFixture, BloomZeroIntensityCopiesSceneOnly)
{
    RVX_REQUIRE_RENDER_RUNTIME_PIPELINE();

    BloomPass pass;
    PostProcessSettings settings;
    settings.enableBloom = true;
    settings.bloomIntensity = 0.0f;
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
    EXPECT_EQ(stats.totalTransientTextures, 0u);

    RecordingCommandContext ctx;
    graph.Execute(ctx);

    ASSERT_EQ(ctx.pipelineSequence.size(), static_cast<size_t>(1));
    EXPECT_EQ(ctx.pipelineSequence[0], pipelineCache.GetBloomPipeline(RHIFormat::RGBA16_FLOAT));
    EXPECT_EQ(ctx.drawCount, 1u);
    ASSERT_EQ(ctx.renderPasses.size(), static_cast<size_t>(1));
    EXPECT_EQ(ctx.renderPasses[0].renderArea.width, 64u);
    EXPECT_EQ(ctx.renderPasses[0].renderArea.height, 64u);
    EXPECT_EQ(ctx.renderPasses[0].colorAttachments[0].loadOp, RHILoadOp::DontCare);
}

TEST_F(RenderPassValidationFixture, BloomSkipsDrawWhenConstantsCannotMap)
{
    RVX_REQUIRE_RENDER_RUNTIME_PIPELINE(false);

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
    RVX_REQUIRE_RENDER_RUNTIME_PIPELINE();

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
    ASSERT_EQ(descriptorIt->bindings.size(), static_cast<size_t>(4));

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
    EXPECT_TRUE(hasBinding(3, false, true, false));
    EXPECT_EQ(descriptorIt->bindings[3].textureView, descriptorIt->bindings[1].textureView);

    auto descriptorCall = std::find(ctx.callSequence.begin(), ctx.callSequence.end(), "SetDescriptorSet");
    auto drawCall = std::find(ctx.callSequence.begin(), ctx.callSequence.end(), "Draw");
    ASSERT_NE(descriptorCall, ctx.callSequence.end());
    ASSERT_NE(drawCall, ctx.callSequence.end());
    EXPECT_LT(std::distance(ctx.callSequence.begin(), descriptorCall),
              std::distance(ctx.callSequence.begin(), drawCall));
}

TEST_F(RenderPassValidationFixture, FXAASkipsDrawWhenConstantsCannotMap)
{
    RVX_REQUIRE_RENDER_RUNTIME_PIPELINE(false);

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
    RVX_REQUIRE_RENDER_RUNTIME_PIPELINE();

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
    RVX_REQUIRE_RENDER_RUNTIME_PIPELINE();

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
    RVX_REQUIRE_RENDER_RUNTIME_PIPELINE();

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
    RVX_REQUIRE_RENDER_RUNTIME_PIPELINE();

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
    RVX_REQUIRE_RENDER_RUNTIME_PIPELINE();

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
    ASSERT_EQ(descriptorIt->bindings.size(), static_cast<size_t>(4));
    EXPECT_EQ(descriptorIt->bindings[3].binding, 3u);
    EXPECT_EQ(descriptorIt->bindings[3].textureView, descriptorIt->bindings[1].textureView);
}

TEST_F(RenderPassValidationFixture, ColorGradingUploadsConstantsWithHLSLPacking)
{
    RVX_REQUIRE_RENDER_RUNTIME_PIPELINE();

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
    RVX_REQUIRE_RENDER_RUNTIME_PIPELINE(false);

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
    RVX_REQUIRE_RENDER_RUNTIME_PIPELINE();

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
    RVX_REQUIRE_RENDER_RUNTIME_PIPELINE();

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
    RVX_REQUIRE_RENDER_RUNTIME_PIPELINE();

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
    ASSERT_EQ(descriptorIt->bindings.size(), static_cast<size_t>(4));
    EXPECT_EQ(descriptorIt->bindings[3].binding, 3u);
    EXPECT_EQ(descriptorIt->bindings[3].textureView, descriptorIt->bindings[1].textureView);
}

TEST_F(RenderPassValidationFixture, ChromaticAberrationUploadsConstantsWithHLSLPacking)
{
    RVX_REQUIRE_RENDER_RUNTIME_PIPELINE();

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
    RVX_REQUIRE_RENDER_RUNTIME_PIPELINE(false);

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
    RVX_REQUIRE_RENDER_RUNTIME_PIPELINE();

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
    ASSERT_EQ(descriptorIt->bindings.size(), static_cast<size_t>(4));

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
    EXPECT_TRUE(hasBinding(3, false, true, false));
    EXPECT_EQ(descriptorIt->bindings[3].textureView, descriptorIt->bindings[1].textureView);
}

TEST_F(RenderPassValidationFixture, VignetteSkipsDrawWhenConstantsCannotMap)
{
    RVX_REQUIRE_RENDER_RUNTIME_PIPELINE(false);

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
    RVX_REQUIRE_RENDER_RUNTIME_PIPELINE();

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
    EXPECT_EQ(graphStats.totalPasses, 8u);

    RecordingCommandContext ctx;
    graph.Execute(ctx);

    ASSERT_EQ(ctx.pipelineSequence.size(), static_cast<size_t>(8));
    for (size_t i = 0; i < 4; ++i)
    {
        ExpectPipelineDebugName(ctx.pipelineSequence[i], "BloomPipeline");
    }
    for (size_t i = 4; i < 7; ++i)
    {
        ExpectPipelineDebugName(ctx.pipelineSequence[i], "BloomAdditivePipeline");
    }
    EXPECT_EQ(ctx.pipelineSequence[7], pipelineCache.GetToneMappingPipeline());
    EXPECT_EQ(ctx.drawCount, 8u);
    EXPECT_EQ(ctx.lastDrawVertexCount, 3u);
    EXPECT_EQ(ctx.beginRenderPassCount, 8u);
    EXPECT_EQ(ctx.endRenderPassCount, 8u);
}

TEST_F(RenderPassValidationFixture, PostProcessStackRunsBloomToneMappingColorGradingChromaticVignetteFXAAWithHDRAndLDRIntermediates)
{
    RVX_REQUIRE_RENDER_RUNTIME_PIPELINE();

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
    EXPECT_EQ(graphStats.totalPasses, 12u);

    RecordingCommandContext ctx;
    graph.Execute(ctx);

    ASSERT_EQ(ctx.pipelineSequence.size(), static_cast<size_t>(12));
    for (size_t i = 0; i < 4; ++i)
    {
        ExpectPipelineDebugName(ctx.pipelineSequence[i], "BloomPipeline");
    }
    for (size_t i = 4; i < 7; ++i)
    {
        ExpectPipelineDebugName(ctx.pipelineSequence[i], "BloomAdditivePipeline");
    }
    EXPECT_EQ(ctx.pipelineSequence[7], pipelineCache.GetToneMappingPipeline());
    EXPECT_EQ(ctx.pipelineSequence[8], pipelineCache.GetColorGradingPipeline(RHIFormat::RGBA8_UNORM));
    EXPECT_EQ(ctx.pipelineSequence[9], pipelineCache.GetChromaticAberrationPipeline(RHIFormat::RGBA8_UNORM));
    EXPECT_EQ(ctx.pipelineSequence[10], pipelineCache.GetVignettePipeline(RHIFormat::RGBA8_UNORM));
    EXPECT_EQ(ctx.pipelineSequence[11], pipelineCache.GetFXAAPipeline(RHIFormat::RGBA8_UNORM));
    EXPECT_EQ(ctx.drawCount, 12u);
    EXPECT_EQ(ctx.lastDrawVertexCount, 3u);
    EXPECT_EQ(ctx.beginRenderPassCount, 12u);
    EXPECT_EQ(ctx.endRenderPassCount, 12u);
}

TEST_F(RenderPassValidationFixture, PostProcessStackKeepsZeroIntensityVignetteAsPassThrough)
{
    RVX_REQUIRE_RENDER_RUNTIME_PIPELINE();

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
    RVX_REQUIRE_RENDER_RUNTIME_PIPELINE();

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
    RVX_REQUIRE_RENDER_RUNTIME_PIPELINE();

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
    RVX_REQUIRE_RENDER_RUNTIME_PIPELINE();

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

TEST_F(RenderPassValidationFixture, PostProcessStackRejectsHDRChainWritingLDROutputWithoutToneMapping)
{
    RVX_REQUIRE_RENDER_RUNTIME_PIPELINE();

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
    auto* bloom = stack.AddEffect<RecordingPostProcessPass>("Bloom", 100);

    stack.Execute(graph, input, output);

    const PostProcessStackExecuteStats& executeStats = stack.GetLastExecuteStats();
    EXPECT_FALSE(executeStats.toneMappingBoundaryValid);
    EXPECT_NE(executeStats.toneMappingBoundaryWarning.find("requires ToneMapping"), std::string::npos);
    EXPECT_EQ(executeStats.enabledEffectCount, 1u);
    EXPECT_EQ(executeStats.graphPassCount, 0u);
    EXPECT_EQ(executeStats.transientIntermediateCount, 0u);
    EXPECT_EQ(executeStats.finalOutputFormat, RHIFormat::RGBA8_UNORM);
    EXPECT_EQ(bloom->addToGraphCount, 0u);

    graph.Compile();
    const auto& graphStats = graph.GetCompileStats();
    EXPECT_TRUE(graphStats.compileValid);
    EXPECT_EQ(graphStats.totalPasses, 0u);
}

TEST_F(RenderPassValidationFixture, PostProcessStackAllowsHDRChainWritingHDROutputWithoutToneMapping)
{
    RVX_REQUIRE_RENDER_RUNTIME_PIPELINE();

    RenderGraph graph;
    graph.SetDevice(&device);

    RHITextureRef inputTexture =
        device.CreateTexture(RHITextureDesc::RenderTarget(32, 32, RHIFormat::RGBA16_FLOAT));
    RHITextureRef outputTexture =
        device.CreateTexture(RHITextureDesc::RenderTarget(32, 32, RHIFormat::RGBA16_FLOAT));
    ASSERT_TRUE(inputTexture);
    ASSERT_TRUE(outputTexture);

    RGTextureHandle input = graph.ImportTexture(inputTexture.Get(), RHIResourceState::ShaderResource);
    RGTextureHandle output = graph.ImportTexture(outputTexture.Get(), RHIResourceState::RenderTarget);
    graph.SetExportState(output, RHIResourceState::RenderTarget);

    PostProcessStack stack;
    auto* bloom = stack.AddEffect<RecordingPostProcessPass>("Bloom", 100);

    stack.Execute(graph, input, output);

    const PostProcessStackExecuteStats& executeStats = stack.GetLastExecuteStats();
    EXPECT_TRUE(executeStats.toneMappingBoundaryValid);
    EXPECT_TRUE(executeStats.toneMappingBoundaryWarning.empty());
    EXPECT_EQ(executeStats.enabledEffectCount, 1u);
    EXPECT_EQ(executeStats.graphPassCount, 1u);
    EXPECT_EQ(executeStats.transientIntermediateCount, 0u);
    EXPECT_EQ(executeStats.finalOutputFormat, RHIFormat::RGBA16_FLOAT);
    EXPECT_EQ(bloom->addToGraphCount, 1u);

    graph.Compile();
    const auto& graphStats = graph.GetCompileStats();
    EXPECT_TRUE(graphStats.compileValid);
    EXPECT_EQ(graphStats.totalPasses, 1u);
}

TEST_F(RenderPassValidationFixture, PostProcessStackReportsInvalidLDREffectBeforeToneMapping)
{
    RVX_REQUIRE_RENDER_RUNTIME_PIPELINE();

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
    RVX_REQUIRE_RENDER_RUNTIME_PIPELINE();

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
    RVX_REQUIRE_RENDER_RUNTIME_PIPELINE();

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
    RVX_REQUIRE_RENDER_RUNTIME_PIPELINE();

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

TEST(RenderPostProcessStackValidation, EvaluateEffectsCountsRuntimeSupportedEffects)
{
    PostProcessStack stack;
    (void)stack.AddEffect<RecordingPostProcessPass>("Bloom", 500);
    (void)stack.AddEffect<RecordingPostProcessPass>("ToneMapping", 900);

    const PostProcessStackExecuteStats stats = stack.EvaluateEffects();
    EXPECT_FALSE(stats.noEffectNoWork);
    EXPECT_EQ(stats.requestedEffectCount, 2u);
    EXPECT_EQ(stats.unsupportedSkippedCount, 0u);
    EXPECT_EQ(stats.enabledEffectCount, 2u);
    EXPECT_EQ(stats.graphPassCount, 0u);
    EXPECT_EQ(stats.transientIntermediateCount, 0u);
}

TEST_F(RenderPassValidationFixture, PostProcessRuntimeResourcesReportSupportedWhenCompilerAvailable)
{
    RVX_REQUIRE_RENDER_RUNTIME_PIPELINE();

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
    stack.Execute(graph, RGTextureHandle{}, RGTextureHandle{});

    const PostProcessStackExecuteStats& executeStats = stack.GetLastExecuteStats();
    EXPECT_TRUE(executeStats.noEffectNoWork);
    EXPECT_EQ(executeStats.enabledEffectCount, 0u);
    EXPECT_EQ(executeStats.graphPassCount, 0u);

    graph.Compile();
    const auto& graphStats = graph.GetCompileStats();
    EXPECT_TRUE(graphStats.compileValid);
    EXPECT_EQ(graphStats.totalPasses, 0u);
}

TEST_F(RenderPassValidationFixture, NoSupportedEffectsCopiesCompatibleSceneColorToOutput)
{
    RenderGraph graph;
    graph.SetDevice(&device);

    RHITextureRef inputTexture =
        device.CreateTexture(RHITextureDesc::RenderTarget(32, 32, RHIFormat::RGBA8_UNORM));
    RHITextureRef outputTexture =
        device.CreateTexture(RHITextureDesc::RenderTarget(32, 32, RHIFormat::RGBA8_UNORM));
    ASSERT_TRUE(inputTexture);
    ASSERT_TRUE(outputTexture);

    RGTextureHandle input = graph.ImportTexture(inputTexture.Get(), RHIResourceState::ShaderResource);
    RGTextureHandle output = graph.ImportTexture(outputTexture.Get(), RHIResourceState::RenderTarget);
    graph.SetExportState(output, RHIResourceState::RenderTarget);

    PostProcessStack stack;
    stack.Execute(graph, input, output);

    const PostProcessStackExecuteStats& executeStats = stack.GetLastExecuteStats();
    EXPECT_TRUE(executeStats.noEffectNoWork);
    EXPECT_TRUE(executeStats.fallbackCopyApplied);
    EXPECT_EQ(executeStats.fallbackCopyPassCount, 1u);
    EXPECT_EQ(executeStats.graphPassCount, 1u);
    EXPECT_EQ(executeStats.finalOutputFormat, RHIFormat::RGBA8_UNORM);

    RecordingCommandContext ctx;
    graph.Execute(ctx);
    EXPECT_EQ(ctx.copyTextureCount, 1u);
}

TEST_F(RenderPassValidationFixture, InvalidToneMappingBoundaryCopiesCompatibleSceneColorToOutput)
{
    RenderGraph graph;
    graph.SetDevice(&device);

    RHITextureRef inputTexture =
        device.CreateTexture(RHITextureDesc::RenderTarget(32, 32, RHIFormat::RGBA8_UNORM));
    RHITextureRef outputTexture =
        device.CreateTexture(RHITextureDesc::RenderTarget(32, 32, RHIFormat::RGBA8_UNORM));
    ASSERT_TRUE(inputTexture);
    ASSERT_TRUE(outputTexture);

    RGTextureHandle input = graph.ImportTexture(inputTexture.Get(), RHIResourceState::ShaderResource);
    RGTextureHandle output = graph.ImportTexture(outputTexture.Get(), RHIResourceState::RenderTarget);
    graph.SetExportState(output, RHIResourceState::RenderTarget);

    PostProcessStack stack;
    (void)stack.AddEffect<RecordingPostProcessPass>("FXAA", 100);
    stack.Execute(graph, input, output);

    const PostProcessStackExecuteStats& executeStats = stack.GetLastExecuteStats();
    EXPECT_FALSE(executeStats.toneMappingBoundaryValid);
    EXPECT_TRUE(executeStats.fallbackCopyApplied);
    EXPECT_EQ(executeStats.fallbackCopyPassCount, 1u);
    EXPECT_EQ(executeStats.graphPassCount, 1u);
    EXPECT_EQ(executeStats.finalOutputFormat, RHIFormat::RGBA8_UNORM);

    RecordingCommandContext ctx;
    graph.Execute(ctx);
    EXPECT_EQ(ctx.copyTextureCount, 1u);
}

TEST(RenderPostProcessStackValidation, MultiPassChainUsesDistinctTransientIntermediate)
{
    FakeDevice device;
    RenderGraph graph;
    graph.SetDevice(&device);

    RHITextureRef inputTexture =
        device.CreateTexture(RHITextureDesc::RenderTarget(32, 32, RHIFormat::RGBA16_FLOAT));
    RHITextureRef outputTexture =
        device.CreateTexture(RHITextureDesc::RenderTarget(32, 32, RHIFormat::RGBA16_FLOAT));
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

TEST_F(RenderPassValidationFixture, ObjectVelocityPassDrawsMaskedItemsWithMaterialSet)
{
    RVX_REQUIRE_RENDER_RUNTIME_PIPELINE();

    RenderObject& object = scene.GetMutableObject(0);
    object.previousWorldMatrix = Mat4Identity();
    object.previousWorldMatrix[3][0] = -1.0f;
    object.previousWorldMatrixValid = 1;

    Resource::MaterialResource materialResource;
    materialResource.SetId(601);
    materialResource.SetName("MaskedVelocityMaterial");
    materialResource.SetMaterialData(std::make_shared<Material>());
    ASSERT_TRUE(gpuResources.UploadImmediate(&materialResource));

    RenderDrawItem maskedItem = MakeDrawItem(MaterialRenderMode::Masked);
    maskedItem.material = gpuResources.GetHandle(materialResource.GetId());
    std::vector<RenderDrawItem> opaqueItems;
    std::vector<RenderDrawItem> maskedItems = {maskedItem};

    RenderGraph graph;
    graph.SetDevice(&device);

    RHITextureDesc velocityDesc = RHITextureDesc::RenderTarget(64, 64, RHIFormat::RG16_FLOAT);
    velocityDesc.debugName = "GraphVelocityForObjectVelocityPass";
    RHITextureRef velocityTexture = device.CreateTexture(velocityDesc);
    ASSERT_TRUE(velocityTexture);
    view.velocityTarget = graph.ImportTexture(velocityTexture.Get(), RHIResourceState::RenderTarget);

    RHITextureDesc depthDesc = RHITextureDesc::DepthStencil(64, 64, PipelineCache::GetDefaultDepthStencilFormat());
    depthDesc.debugName = "GraphDepthForObjectVelocityPass";
    RHITextureRef depthTexture = device.CreateTexture(depthDesc);
    ASSERT_TRUE(depthTexture);
    view.depthTarget = graph.ImportTexture(depthTexture.Get(), RHIResourceState::DepthRead);

    view.renderGraph = &graph;
    view.viewCache = &viewCache;
    view.viewportWidth = 64;
    view.viewportHeight = 64;
    view.previousViewProjectionMatrix = Mat4Identity();
    view.previousViewProjectionValid = 1;
    view.resetTemporalHistory = false;

    ObjectVelocityPass pass;
    ConfigureResources(pass, gpuResources, pipelineCache, viewCache, materialSystem);
    pass.SetEnabled(true);

    ASSERT_TRUE(pass.IsSupported()) << pass.GetUnsupportedReason();

    RenderPassRecordContext context;
    context.view = view;
    context.identity.graph = &graph;
    context.identity.graphIdentity = graph.GetGraphIdentity();
    context.identity.graphRecordingGeneration = graph.GetRecordingGeneration();
    context.identity.frameSequence = 1;
    context.identity.viewOrdinal = 0;
    context.identity.recordEpoch = 1;
    context.renderScene = &scene;
    context.opaqueDrawItems = &opaqueItems;
    context.maskedDrawItems = &maskedItems;
    context.results = std::make_shared<RenderPassRecordResults>();
    context.frameSnapshot = MakeRenderPassFrameSnapshot(context, *context.results);
    pass.AddToGraph(graph, context);
    graph.Compile();
    EXPECT_TRUE(graph.GetCompileStats().compileValid);
    EXPECT_EQ(graph.GetCompileStats().totalPasses, 1u);

    RecordingCommandContext ctx;
    graph.Execute(ctx);
    pass.PublishRecordResults(context.results, context.identity);

    ASSERT_EQ(ctx.pipelineSequence.size(), static_cast<size_t>(1));
    EXPECT_EQ(ctx.pipelineSequence[0], pipelineCache.GetMaskedObjectVelocityPipeline(RHIFormat::RG16_FLOAT));
    EXPECT_EQ(1u, ctx.drawIndexedCount);
    EXPECT_TRUE(std::any_of(ctx.descriptorSetSequence.begin(), ctx.descriptorSetSequence.end(),
                            [](uint32 set) { return set == 2; }));

    const ObjectVelocityPassStats& stats = pass.GetStats();
    EXPECT_TRUE(stats.velocityRecorded);
    EXPECT_EQ(stats.opaqueDrawItemCount, 0u);
    EXPECT_EQ(stats.maskedDrawItemCount, 1u);
    EXPECT_EQ(stats.maskedDrawCount, 1u);
    EXPECT_EQ(stats.skippedMissingUVCount, 0u);
    EXPECT_EQ(stats.skippedMaterialBindingCount, 0u);
}

TEST_F(RenderPassValidationFixture,
       ObjectVelocityPassOwnsRecordingBindingsAcrossReverseGraphs)
{
    RVX_REQUIRE_RENDER_RUNTIME_PIPELINE();
    device.SetFenceAutoComplete(false);

    RenderSubmissionTracker tracker;
    ASSERT_TRUE(tracker.Initialize(&device));
    RenderRetirementQueue retirement;
    ASSERT_TRUE(retirement.Initialize(&tracker));

    Resource::MaterialResource materialResource;
    materialResource.SetId(702);
    materialResource.SetName("ObjectVelocityRecordingMaterial");
    materialResource.SetMaterialData(std::make_shared<Material>());
    ASSERT_TRUE(gpuResources.UploadImmediate(&materialResource));
    const RenderResourceHandle materialHandle =
        gpuResources.GetHandle(materialResource.GetId());
    ASSERT_TRUE(materialHandle.IsValid());

    RenderObject objectA = scene.GetObject(0);
    objectA.worldMatrix = Mat4Identity();
    objectA.worldMatrix[3][0] = 2.0f;
    objectA.previousWorldMatrix = Mat4Identity();
    objectA.previousWorldMatrix[3][0] = -2.0f;
    objectA.previousWorldMatrixValid = 1;
    RenderScene sceneA;
    sceneA.AddObject(objectA);

    RenderObject objectB = objectA;
    objectB.worldMatrix[3][0] = 7.0f;
    objectB.previousWorldMatrix[3][0] = 5.0f;
    RenderScene sceneB;
    sceneB.AddObject(objectB);

    RenderDrawItem opaqueA = MakeDrawItem(MaterialRenderMode::Opaque);
    RenderDrawItem maskedA = MakeDrawItem(MaterialRenderMode::Masked);
    maskedA.material = materialHandle;
    RenderDrawItem maskedB = maskedA;
    std::vector<RenderDrawItem> opaqueItemsA = {opaqueA};
    std::vector<RenderDrawItem> maskedItemsA = {maskedA};
    std::vector<RenderDrawItem> opaqueItemsB;
    std::vector<RenderDrawItem> maskedItemsB = {maskedB};

    struct VelocityTargets
    {
        RHITextureRef velocity;
        RHITextureRef depth;
    };
    const auto makeContext = [&] (RenderGraph& graph,
                                   const RenderScene& recordScene,
                                   const std::vector<RenderDrawItem>& opaqueItems,
                                   const std::vector<RenderDrawItem>& maskedItems,
                                   RenderSubmissionResourceBatch& batch,
                                   VelocityTargets& targets,
                                   uint64 frameSequence,
                                   uint64 recordEpoch,
                                   uint32 extent,
                                   float viewTranslation)
    {
        RHITextureDesc velocityDesc = RHITextureDesc::RenderTarget(
            extent, extent, RHIFormat::RG16_FLOAT);
        velocityDesc.debugName = "ObjectVelocityRecordTarget";
        targets.velocity = device.CreateTexture(velocityDesc);
        EXPECT_TRUE(targets.velocity);
        RHITextureDesc depthDesc = RHITextureDesc::DepthStencil(
            extent, extent, PipelineCache::GetDefaultDepthStencilFormat());
        depthDesc.debugName = "ObjectVelocityRecordDepth";
        targets.depth = device.CreateTexture(depthDesc);
        EXPECT_TRUE(targets.depth);

        RenderPassRecordContext context;
        context.view.renderGraph = &graph;
        context.view.viewCache = &viewCache;
        context.view.submissionResourceBatch = &batch;
        context.view.velocityTarget = graph.ImportTexture(
            targets.velocity.Get(), RHIResourceState::RenderTarget);
        context.view.depthTarget = graph.ImportTexture(
            targets.depth.Get(), RHIResourceState::DepthRead);
        context.view.viewportWidth = extent;
        context.view.viewportHeight = extent;
        context.view.viewProjectionMatrix = Mat4Identity();
        context.view.viewProjectionMatrix[3][0] = viewTranslation;
        context.view.previousViewProjectionMatrix = Mat4Identity();
        context.view.previousViewProjectionMatrix[3][0] = -viewTranslation;
        context.view.previousViewProjectionValid = 1;
        context.view.resetTemporalHistory = false;
        context.identity.graph = &graph;
        context.identity.graphIdentity = graph.GetGraphIdentity();
        context.identity.graphRecordingGeneration = graph.GetRecordingGeneration();
        context.identity.frameSequence = frameSequence;
        context.identity.viewOrdinal = 0;
        context.identity.recordEpoch = recordEpoch;
        context.renderScene = &recordScene;
        context.opaqueDrawItems = &opaqueItems;
        context.maskedDrawItems = &maskedItems;
        context.results = std::make_shared<RenderPassRecordResults>();
        context.frameSnapshot = MakeRenderPassFrameSnapshot(
            context, *context.results);
        return context;
    };

    RenderGraph graphA;
    RenderGraph graphB;
    graphA.SetDevice(&device);
    graphB.SetDevice(&device);
    RenderSubmissionResourceBatch batchA;
    RenderSubmissionResourceBatch batchB;
    VelocityTargets targetsA;
    VelocityTargets targetsB;
    RenderPassRecordContext contextA = makeContext(
        graphA, sceneA, opaqueItemsA, maskedItemsA, batchA, targetsA,
        501, 501, 64, 3.0f);
    RenderPassRecordContext contextB = makeContext(
        graphB, sceneB, opaqueItemsB, maskedItemsB, batchB, targetsB,
        502, 502, 96, 9.0f);

    ObjectVelocityPass pass;
    ConfigureResources(pass, gpuResources, pipelineCache, viewCache, materialSystem);
    pass.SetEnabled(true);
    ASSERT_TRUE(pass.IsSupported()) << pass.GetUnsupportedReason();

    // DX12/Vulkan/Metal descriptor sets and pipelines currently carry only
    // raw layout identities. Pin probes let this runtime test prove both graph
    // recording and submission retirement keep those layout owners alive.
    ASSERT_EQ(3u, pipelineCache.m_setLayouts.size());
    RHIPipelineLayoutRef pipelineLayoutProbe(pipelineCache.m_pipelineLayout.Get());
    std::array<RHIDescriptorSetLayoutRef, 3> setLayoutProbes = {
        RHIDescriptorSetLayoutRef(pipelineCache.m_setLayouts[0].Get()),
        RHIDescriptorSetLayoutRef(pipelineCache.m_setLayouts[1].Get()),
        RHIDescriptorSetLayoutRef(pipelineCache.m_setLayouts[2].Get())};
    ASSERT_TRUE(pipelineLayoutProbe);
    ASSERT_TRUE(setLayoutProbes[0]);
    ASSERT_TRUE(setLayoutProbes[1]);
    ASSERT_TRUE(setLayoutProbes[2]);
    const uint32 pipelineLayoutReferencesBeforeRecording =
        pipelineLayoutProbe->GetRefCount();
    const uint32 frameSetLayoutReferencesBeforeRecording =
        setLayoutProbes[0]->GetRefCount();
    const uint32 objectSetLayoutReferencesBeforeRecording =
        setLayoutProbes[1]->GetRefCount();
    const uint32 materialSetLayoutReferencesBeforeRecording =
        setLayoutProbes[2]->GetRefCount();
    pass.AddToGraph(graphA, contextA);
    pass.AddToGraph(graphB, contextB);

    // Each graph owns a raster snapshot and its batch owns a retirement
    // reference. The material layout additionally has one material snapshot
    // per graph; the batch deduplicates it with the raster snapshot's set 2.
    EXPECT_EQ(pipelineLayoutReferencesBeforeRecording + 4u,
              pipelineLayoutProbe->GetRefCount());
    EXPECT_EQ(frameSetLayoutReferencesBeforeRecording + 4u,
              setLayoutProbes[0]->GetRefCount());
    EXPECT_EQ(objectSetLayoutReferencesBeforeRecording + 4u,
              setLayoutProbes[1]->GetRefCount());
    EXPECT_EQ(materialSetLayoutReferencesBeforeRecording + 6u,
              setLayoutProbes[2]->GetRefCount());

    std::vector<FakeBuffer*> recordViewBuffers;
    std::vector<FakeBuffer*> recordObjectBuffers;
    std::vector<FakeBuffer*> recordMaterialBuffers;
    for (size_t i = 0; i < device.createdBufferDescs.size(); ++i)
    {
        const char* debugName = device.createdBufferDescs[i].debugName;
        if (!debugName)
            continue;
        const std::string name(debugName);
        if (name == "ObjectVelocityRecordViewConstants")
            recordViewBuffers.push_back(device.createdBuffers[i]);
        else if (name == "ObjectVelocityRecordObjectConstants")
            recordObjectBuffers.push_back(device.createdBuffers[i]);
        else if (name == "ObjectVelocityRecordMaterialConstants")
            recordMaterialBuffers.push_back(device.createdBuffers[i]);
    }
    ASSERT_EQ(2u, recordViewBuffers.size());
    ASSERT_EQ(2u, recordObjectBuffers.size());
    ASSERT_EQ(2u, recordMaterialBuffers.size());

    ViewConstants recordedViewA{};
    ViewConstants recordedViewB{};
    std::memcpy(&recordedViewA, recordViewBuffers[0]->GetStorage().data(),
                sizeof(recordedViewA));
    std::memcpy(&recordedViewB, recordViewBuffers[1]->GetStorage().data(),
                sizeof(recordedViewB));
    EXPECT_FLOAT_EQ(3.0f, recordedViewA.viewProjection[3][0]);
    EXPECT_FLOAT_EQ(9.0f, recordedViewB.viewProjection[3][0]);

    const uint64 objectStride = pipelineCache.m_objectConstantStride;
    ASSERT_GT(objectStride, 0u);
    ObjectConstants recordedObjectA0{};
    ObjectConstants recordedObjectA1{};
    ObjectConstants recordedObjectB{};
    std::memcpy(&recordedObjectA0, recordObjectBuffers[0]->GetStorage().data(),
                sizeof(recordedObjectA0));
    std::memcpy(&recordedObjectA1,
                recordObjectBuffers[0]->GetStorage().data() + objectStride,
                sizeof(recordedObjectA1));
    std::memcpy(&recordedObjectB, recordObjectBuffers[1]->GetStorage().data(),
                sizeof(recordedObjectB));
    EXPECT_FLOAT_EQ(2.0f, recordedObjectA0.world[3][0]);
    EXPECT_FLOAT_EQ(2.0f, recordedObjectA1.world[3][0]);
    EXPECT_FLOAT_EQ(7.0f, recordedObjectB.world[3][0]);

    // Setup already consumed value-owned scene/list/view state.  Mutating every
    // caller-owned input before either graph runs must not alter its recording.
    sceneA.GetMutableObject(0).worldMatrix[3][0] = 99.0f;
    opaqueItemsA.clear();
    maskedItemsA.clear();
    contextA.view.viewProjectionMatrix[3][0] = 99.0f;

    graphA.Compile();
    graphB.Compile();
    ASSERT_TRUE(graphA.GetCompileStats().compileValid);
    ASSERT_TRUE(graphB.GetCompileStats().compileValid);

    const RenderGraph::Diagnostics graphDiagnostics = graphA.GetDiagnostics();
    const auto velocityPassDiagnostic = std::find_if(
        graphDiagnostics.passes.begin(), graphDiagnostics.passes.end(),
        [](const RenderGraph::PassDiagnostic& diagnostic)
        {
            return diagnostic.name == "ObjectVelocityPass";
        });
    ASSERT_NE(graphDiagnostics.passes.end(), velocityPassDiagnostic);
    const auto velocityUsage = std::find_if(
        velocityPassDiagnostic->usages.begin(), velocityPassDiagnostic->usages.end(),
        [&contextA](const RenderGraph::ResourceUsageDiagnostic& usage)
        {
            return usage.type == RenderGraph::DiagnosticResourceType::Texture &&
                   usage.resourceIndex == contextA.view.velocityTarget.index;
        });
    const auto depthUsage = std::find_if(
        velocityPassDiagnostic->usages.begin(), velocityPassDiagnostic->usages.end(),
        [&contextA](const RenderGraph::ResourceUsageDiagnostic& usage)
        {
            return usage.type == RenderGraph::DiagnosticResourceType::Texture &&
                   usage.resourceIndex == contextA.view.depthTarget.index;
        });
    ASSERT_NE(velocityPassDiagnostic->usages.end(), velocityUsage);
    ASSERT_NE(velocityPassDiagnostic->usages.end(), depthUsage);
    EXPECT_EQ(RenderGraph::DiagnosticAccessType::ReadWrite, velocityUsage->access);
    EXPECT_EQ(RHIResourceState::RenderTarget, velocityUsage->desiredState);
    EXPECT_EQ(RenderGraph::DiagnosticAccessType::Read, depthUsage->access);
    EXPECT_EQ(RHIResourceState::DepthRead, depthUsage->desiredState);

    const uint32 retainedBeforeB = batchB.GetRetainedObjectCount();
    RecordingCommandContext commandsB;
    graphB.Execute(commandsB);
    EXPECT_EQ(1u, commandsB.beginRenderPassCount);
    EXPECT_EQ(1u, commandsB.drawIndexedCount);
    // Execute-time RTV/DSV retention contributes view + parent texture for
    // each attachment, including backends whose views do not own textures.
    EXPECT_EQ(retainedBeforeB + 4u, batchB.GetRetainedObjectCount());
    pass.PublishRecordResults(contextB.results, contextB.identity);
    const ObjectVelocityPassStats publishedB = pass.GetStats();
    EXPECT_TRUE(publishedB.velocityRecorded);
    EXPECT_EQ(96u, publishedB.width);
    EXPECT_EQ(1u, publishedB.maskedDrawCount);

    const uint32 retainedBeforeA = batchA.GetRetainedObjectCount();
    RecordingCommandContext commandsA;
    graphA.Execute(commandsA);
    EXPECT_EQ(1u, commandsA.beginRenderPassCount);
    EXPECT_EQ(2u, commandsA.drawIndexedCount);
    EXPECT_EQ(retainedBeforeA + 4u, batchA.GetRetainedObjectCount());
    ASSERT_EQ(1u, commandsA.renderPasses.size());
    ASSERT_EQ(1u, commandsB.renderPasses.size());
    EXPECT_EQ(targetsA.velocity.Get(),
              commandsA.renderPasses[0].colorAttachments[0].view->GetTexture());
    EXPECT_EQ(targetsB.velocity.Get(),
              commandsB.renderPasses[0].colorAttachments[0].view->GetTexture());

    // Publishing delayed A must not regress diagnostics from newer B.
    pass.PublishRecordResults(contextA.results, contextA.identity);
    EXPECT_EQ(publishedB.width, pass.GetStats().width);
    EXPECT_EQ(publishedB.maskedDrawCount, pass.GetStats().maskedDrawCount);

    const auto findDescriptorCall = [](
        const RecordingCommandContext& commands,
        uint32 set,
        uint32 occurrence = 0) -> size_t
    {
        uint32 found = 0;
        for (size_t i = 0; i < commands.descriptorSetSequence.size(); ++i)
        {
            if (commands.descriptorSetSequence[i] != set)
                continue;
            if (found++ == occurrence)
                return i;
        }
        return std::numeric_limits<size_t>::max();
    };
    const size_t frameAIndex = findDescriptorCall(commandsA, 0);
    const size_t objectA0Index = findDescriptorCall(commandsA, 1, 0);
    const size_t objectA1Index = findDescriptorCall(commandsA, 1, 1);
    const size_t materialAIndex = findDescriptorCall(commandsA, 2);
    const size_t frameBIndex = findDescriptorCall(commandsB, 0);
    const size_t objectBIndex = findDescriptorCall(commandsB, 1);
    const size_t materialBIndex = findDescriptorCall(commandsB, 2);
    ASSERT_NE(std::numeric_limits<size_t>::max(), frameAIndex);
    ASSERT_NE(std::numeric_limits<size_t>::max(), objectA0Index);
    ASSERT_NE(std::numeric_limits<size_t>::max(), objectA1Index);
    ASSERT_NE(std::numeric_limits<size_t>::max(), materialAIndex);
    ASSERT_NE(std::numeric_limits<size_t>::max(), frameBIndex);
    ASSERT_NE(std::numeric_limits<size_t>::max(), objectBIndex);
    ASSERT_NE(std::numeric_limits<size_t>::max(), materialBIndex);
    EXPECT_NE(commandsA.descriptorSetPointers[frameAIndex],
              commandsB.descriptorSetPointers[frameBIndex]);
    EXPECT_NE(commandsA.descriptorSetPointers[objectA0Index],
              commandsB.descriptorSetPointers[objectBIndex]);
    EXPECT_NE(commandsA.descriptorSetPointers[materialAIndex],
              commandsB.descriptorSetPointers[materialBIndex]);
    EXPECT_NE(commandsA.descriptorSetPointers[frameAIndex],
              pipelineCache.GetFrameDescriptorSet());
    EXPECT_NE(commandsA.descriptorSetPointers[objectA0Index],
              pipelineCache.GetObjectDescriptorSet());
    ASSERT_EQ(1u, commandsA.descriptorSetDynamicOffsets[objectA0Index].size());
    ASSERT_EQ(1u, commandsA.descriptorSetDynamicOffsets[objectA1Index].size());
    ASSERT_EQ(1u, commandsA.descriptorSetDynamicOffsets[materialAIndex].size());
    EXPECT_EQ(0u, commandsA.descriptorSetDynamicOffsets[objectA0Index][0]);
    EXPECT_EQ(static_cast<uint32>(objectStride),
              commandsA.descriptorSetDynamicOffsets[objectA1Index][0]);
    EXPECT_EQ(0u, commandsA.descriptorSetDynamicOffsets[materialAIndex][0]);
    EXPECT_EQ(0u, commandsB.descriptorSetDynamicOffsets[objectBIndex][0]);
    EXPECT_EQ(0u, commandsB.descriptorSetDynamicOffsets[materialBIndex][0]);

    const auto findBufferBinding = [](RHIDescriptorSet* descriptorSet,
                                      uint32 binding) -> RHIBuffer*
    {
        const auto* fakeSet = dynamic_cast<const FakeDescriptorSet*>(descriptorSet);
        if (!fakeSet)
            return nullptr;
        const auto found = std::find_if(
            fakeSet->bindings.begin(), fakeSet->bindings.end(),
            [binding](const RHIDescriptorBinding& candidate)
            {
                return candidate.binding == binding && candidate.arrayElement == 0;
            });
        return found != fakeSet->bindings.end() ? found->buffer : nullptr;
    };
    EXPECT_EQ(recordViewBuffers[0],
              findBufferBinding(commandsA.descriptorSetPointers[frameAIndex], 0));
    EXPECT_EQ(recordObjectBuffers[0],
              findBufferBinding(commandsA.descriptorSetPointers[objectA0Index], 0));
    EXPECT_EQ(recordMaterialBuffers[0],
              findBufferBinding(commandsA.descriptorSetPointers[materialAIndex], 0));
    EXPECT_EQ(recordViewBuffers[1],
              findBufferBinding(commandsB.descriptorSetPointers[frameBIndex], 0));
    EXPECT_EQ(recordObjectBuffers[1],
              findBufferBinding(commandsB.descriptorSetPointers[objectBIndex], 0));
    EXPECT_EQ(recordMaterialBuffers[1],
              findBufferBinding(commandsB.descriptorSetPointers[materialBIndex], 0));

    GPUCompletionToken completionB;
    GPUCompletionToken completionA;
    ASSERT_TRUE(InsertGPUCompletionPoint(completionB, tracker.Submit(&commandsB)));
    ASSERT_TRUE(InsertGPUCompletionPoint(completionA, tracker.Submit(&commandsA)));
    batchB.SealAndTransfer(completionB, retirement);
    batchA.SealAndTransfer(completionA, retirement);
    EXPECT_TRUE(batchA.IsSealed());
    EXPECT_TRUE(batchB.IsSealed());
    EXPECT_EQ(0u, batchA.GetRetainedObjectCount());
    EXPECT_EQ(0u, batchB.GetRetainedObjectCount());
    ASSERT_GT(retirement.GetDiagnostics().entryCount, 0u);

    // Simulate a cache replacement after command submission. Graph callbacks
    // and the cache release their ownership, leaving the in-flight submission
    // batches as the sole owners besides these test probes.
    graphA.Clear();
    graphB.Clear();
    pipelineCache.Shutdown();
    EXPECT_EQ(3u, pipelineLayoutProbe->GetRefCount());
    EXPECT_EQ(3u, setLayoutProbes[0]->GetRefCount());
    EXPECT_EQ(3u, setLayoutProbes[1]->GetRefCount());
    EXPECT_EQ(3u, setLayoutProbes[2]->GetRefCount());
    FakeFence* const completionFence =
        device.FindFenceWithSignal(completionA.points[0].value);
    ASSERT_NE(nullptr, completionFence);
    completionFence->Complete(completionB.points[0].value);
    EXPECT_EQ(GPUCompletionStatus::Pending, retirement.Poll());
    // B's retirement entries have now released, while A remains in flight.
    EXPECT_EQ(2u, pipelineLayoutProbe->GetRefCount());
    EXPECT_EQ(2u, setLayoutProbes[0]->GetRefCount());
    EXPECT_EQ(2u, setLayoutProbes[1]->GetRefCount());
    EXPECT_EQ(2u, setLayoutProbes[2]->GetRefCount());
    completionFence->Complete(completionA.points[0].value);
    EXPECT_EQ(GPUCompletionStatus::Completed, retirement.Poll());
    EXPECT_EQ(1u, pipelineLayoutProbe->GetRefCount());
    EXPECT_EQ(1u, setLayoutProbes[0]->GetRefCount());
    EXPECT_EQ(1u, setLayoutProbes[1]->GetRefCount());
    EXPECT_EQ(1u, setLayoutProbes[2]->GetRefCount());
    tracker.Shutdown();
}

TEST_F(RenderPassValidationFixture,
       ObjectVelocityPassFailsClosedForForeignStaleAndRejectedRecordingInputs)
{
    RVX_REQUIRE_RENDER_RUNTIME_PIPELINE();

    RenderObject& object = scene.GetMutableObject(0);
    object.previousWorldMatrix = Mat4Identity();
    object.previousWorldMatrix[3][0] = -1.0f;
    object.previousWorldMatrixValid = 1;
    std::vector<RenderDrawItem> opaqueItems = {
        MakeDrawItem(MaterialRenderMode::Opaque)};
    std::vector<RenderDrawItem> maskedItems;

    ObjectVelocityPass pass;
    ConfigureResources(pass, gpuResources, pipelineCache, viewCache, materialSystem);
    pass.SetEnabled(true);
    ASSERT_TRUE(pass.IsSupported()) << pass.GetUnsupportedReason();

    const auto makeTargets = [&](uint32 extent,
                                 const char* velocityName,
                                 const char* depthName)
    {
        RHITextureDesc velocityDesc = RHITextureDesc::RenderTarget(
            extent, extent, RHIFormat::RG16_FLOAT);
        velocityDesc.debugName = velocityName;
        RHITextureDesc depthDesc = RHITextureDesc::DepthStencil(
            extent, extent, PipelineCache::GetDefaultDepthStencilFormat());
        depthDesc.debugName = depthName;
        return std::pair<RHITextureRef, RHITextureRef>(
            device.CreateTexture(velocityDesc), device.CreateTexture(depthDesc));
    };
    const auto makeContext = [&](RenderGraph& graph,
                                 RGTextureHandle velocity,
                                 RGTextureHandle depth,
                                 uint64 frameSequence,
                                 uint64 recordEpoch,
                                 bool hasPreviousView,
                                 RenderSubmissionResourceBatch* batch = nullptr)
    {
        RenderPassRecordContext context;
        context.view.renderGraph = &graph;
        context.view.viewCache = &viewCache;
        context.view.submissionResourceBatch = batch;
        context.view.velocityTarget = velocity;
        context.view.depthTarget = depth;
        context.view.viewportWidth = 64;
        context.view.viewportHeight = 64;
        context.view.previousViewProjectionMatrix = Mat4Identity();
        context.view.previousViewProjectionValid = hasPreviousView ? 1 : 0;
        context.view.resetTemporalHistory = !hasPreviousView;
        context.identity.graph = &graph;
        context.identity.graphIdentity = graph.GetGraphIdentity();
        context.identity.graphRecordingGeneration = graph.GetRecordingGeneration();
        context.identity.frameSequence = frameSequence;
        context.identity.viewOrdinal = 0;
        context.identity.recordEpoch = recordEpoch;
        context.renderScene = &scene;
        context.opaqueDrawItems = &opaqueItems;
        context.maskedDrawItems = &maskedItems;
        context.results = std::make_shared<RenderPassRecordResults>();
        context.frameSnapshot = MakeRenderPassFrameSnapshot(
            context, *context.results);
        return context;
    };

    RenderGraph noHistoryGraph;
    noHistoryGraph.SetDevice(&device);
    auto noHistoryTargets = makeTargets(
        64, "ObjectVelocityNoHistoryVelocity", "ObjectVelocityNoHistoryDepth");
    ASSERT_TRUE(noHistoryTargets.first);
    ASSERT_TRUE(noHistoryTargets.second);
    RenderPassRecordContext noHistory = makeContext(
        noHistoryGraph,
        noHistoryGraph.ImportTexture(noHistoryTargets.first.Get(),
                                     RHIResourceState::RenderTarget),
        noHistoryGraph.ImportTexture(noHistoryTargets.second.Get(),
                                     RHIResourceState::DepthRead),
        610, 610, false);
    pass.AddToGraph(noHistoryGraph, noHistory);
    noHistoryGraph.Compile();
    ASSERT_TRUE(noHistoryGraph.GetCompileStats().compileValid);
    RecordingCommandContext noHistoryCommands;
    noHistoryGraph.Execute(noHistoryCommands);
    EXPECT_EQ(0u, noHistoryCommands.beginRenderPassCount);
    EXPECT_EQ(0u, noHistoryCommands.drawIndexedCount);
    EXPECT_FALSE(noHistory.results->objectVelocityStats.outputDeclared);
    EXPECT_FALSE(noHistory.results->objectVelocityStats.velocityRecorded);

    // Valid temporal data is still insufficient without any planned scene
    // items; the pass must not open an attachment-only render pass.
    RenderGraph emptyGraph;
    emptyGraph.SetDevice(&device);
    auto emptyTargets = makeTargets(
        64, "ObjectVelocityEmptyVelocity", "ObjectVelocityEmptyDepth");
    ASSERT_TRUE(emptyTargets.first);
    ASSERT_TRUE(emptyTargets.second);
    std::vector<RenderDrawItem> emptyOpaqueItems;
    std::vector<RenderDrawItem> emptyMaskedItems;
    RenderPassRecordContext empty = noHistory;
    empty.view.renderGraph = &emptyGraph;
    empty.view.velocityTarget = emptyGraph.ImportTexture(
        emptyTargets.first.Get(), RHIResourceState::RenderTarget);
    empty.view.depthTarget = emptyGraph.ImportTexture(
        emptyTargets.second.Get(), RHIResourceState::DepthRead);
    empty.view.previousViewProjectionValid = 1;
    empty.view.resetTemporalHistory = false;
    empty.identity.graph = &emptyGraph;
    empty.identity.graphIdentity = emptyGraph.GetGraphIdentity();
    empty.identity.graphRecordingGeneration = emptyGraph.GetRecordingGeneration();
    empty.identity.frameSequence = 611;
    empty.identity.recordEpoch = 611;
    empty.opaqueDrawItems = &emptyOpaqueItems;
    empty.maskedDrawItems = &emptyMaskedItems;
    empty.results = std::make_shared<RenderPassRecordResults>();
    empty.frameSnapshot = MakeRenderPassFrameSnapshot(empty, *empty.results);
    pass.AddToGraph(emptyGraph, empty);
    emptyGraph.Compile();
    ASSERT_TRUE(emptyGraph.GetCompileStats().compileValid);
    RecordingCommandContext emptyCommands;
    emptyGraph.Execute(emptyCommands);
    EXPECT_EQ(0u, emptyCommands.beginRenderPassCount);
    EXPECT_EQ(0u, emptyCommands.drawIndexedCount);
    EXPECT_FALSE(empty.results->objectVelocityStats.outputDeclared);
    EXPECT_FALSE(empty.results->objectVelocityStats.velocityRecorded);

    // The untyped ViewData overload carries no scene/list recording inputs.
    // It remains a bounded compatibility adapter and must fail closed.
    RenderGraph legacyGraph;
    legacyGraph.SetDevice(&device);
    auto legacyTargets = makeTargets(
        64, "ObjectVelocityLegacyVelocity", "ObjectVelocityLegacyDepth");
    ASSERT_TRUE(legacyTargets.first);
    ASSERT_TRUE(legacyTargets.second);
    ViewData legacyView = empty.view;
    legacyView.renderGraph = &legacyGraph;
    legacyView.velocityTarget = legacyGraph.ImportTexture(
        legacyTargets.first.Get(), RHIResourceState::RenderTarget);
    legacyView.depthTarget = legacyGraph.ImportTexture(
        legacyTargets.second.Get(), RHIResourceState::DepthRead);
    pass.AddToGraph(legacyGraph, legacyView);
    legacyGraph.Compile();
    ASSERT_TRUE(legacyGraph.GetCompileStats().compileValid);
    RecordingCommandContext legacyCommands;
    legacyGraph.Execute(legacyCommands);
    EXPECT_EQ(0u, legacyCommands.beginRenderPassCount);
    EXPECT_EQ(0u, legacyCommands.drawIndexedCount);

    // A results payload owned by graph A must remain wholly untouched when a
    // malformed graph B context attempts to smuggle it into this pass.
    RenderGraph foreignGraph;
    RenderGraph attackerGraph;
    foreignGraph.SetDevice(&device);
    attackerGraph.SetDevice(&device);
    RenderPassRecordIdentity foreignIdentity;
    foreignIdentity.graph = &foreignGraph;
    foreignIdentity.graphIdentity = foreignGraph.GetGraphIdentity();
    foreignIdentity.graphRecordingGeneration = foreignGraph.GetRecordingGeneration();
    foreignIdentity.frameSequence = 620;
    foreignIdentity.recordEpoch = 620;
    auto foreignResults = std::make_shared<RenderPassRecordResults>();
    foreignResults->identity = foreignIdentity;
    foreignResults->objectVelocityStats.width = 777;
    foreignResults->objectVelocityStats.velocityRecorded = true;
    RenderPassRecordContext attacker;
    attacker.view.renderGraph = &attackerGraph;
    attacker.identity.graph = &attackerGraph;
    attacker.identity.graphIdentity = attackerGraph.GetGraphIdentity();
    attacker.identity.graphRecordingGeneration = attackerGraph.GetRecordingGeneration();
    attacker.identity.frameSequence = 621;
    attacker.identity.recordEpoch = 621;
    attacker.results = foreignResults;
    pass.AddToGraph(attackerGraph, attacker);
    attackerGraph.Compile();
    ASSERT_TRUE(attackerGraph.GetCompileStats().compileValid);
    RecordingCommandContext attackerCommands;
    attackerGraph.Execute(attackerCommands);
    EXPECT_EQ(foreignIdentity, foreignResults->identity);
    EXPECT_EQ(777u, foreignResults->objectVelocityStats.width);
    EXPECT_TRUE(foreignResults->objectVelocityStats.velocityRecorded);
    EXPECT_EQ(0u, attackerCommands.beginRenderPassCount);

    // Even a self-consistent graph A source must not invoke the generic
    // helper when registered into graph B with its paired snapshot missing:
    // helper synthesis initializes the supplied results object, which belongs
    // exclusively to graph A. The target graph must be a true no-op.
    RenderGraph sourceSentinelGraph;
    RenderGraph targetSentinelGraph;
    sourceSentinelGraph.SetDevice(&device);
    targetSentinelGraph.SetDevice(&device);
    auto sourceSentinelTargets = makeTargets(
        64, "ObjectVelocitySourceSentinelVelocity", "ObjectVelocitySourceSentinelDepth");
    ASSERT_TRUE(sourceSentinelTargets.first);
    ASSERT_TRUE(sourceSentinelTargets.second);
    RenderPassRecordContext sourceSentinel = makeContext(
        sourceSentinelGraph,
        sourceSentinelGraph.ImportTexture(sourceSentinelTargets.first.Get(),
                                          RHIResourceState::RenderTarget),
        sourceSentinelGraph.ImportTexture(sourceSentinelTargets.second.Get(),
                                          RHIResourceState::DepthRead),
        625, 625, true);
    const RenderPassRecordIdentity sourceSentinelIdentity = sourceSentinel.identity;
    sourceSentinel.results->directionalShadowOutput.identity = sourceSentinelIdentity;
    sourceSentinel.results->directionalShadowOutput.enabled = true;
    sourceSentinel.results->directionalShadowOutput.shadowMapSize = 779;
    sourceSentinel.results->objectVelocityStats.width = 780;
    sourceSentinel.results->objectVelocityStats.velocityRecorded = true;
    sourceSentinel.frameSnapshot.reset();
    pass.AddToGraph(targetSentinelGraph, sourceSentinel);
    targetSentinelGraph.Compile();
    ASSERT_TRUE(targetSentinelGraph.GetCompileStats().compileValid);
    const RenderGraph::Diagnostics targetSentinelDiagnostics =
        targetSentinelGraph.GetDiagnostics();
    const auto targetSentinelPass = std::find_if(
        targetSentinelDiagnostics.passes.begin(), targetSentinelDiagnostics.passes.end(),
        [](const RenderGraph::PassDiagnostic& diagnostic)
        {
            return diagnostic.name == "ObjectVelocityPass";
        });
    ASSERT_NE(targetSentinelDiagnostics.passes.end(), targetSentinelPass);
    EXPECT_TRUE(targetSentinelPass->usages.empty());
    RecordingCommandContext targetSentinelCommands;
    targetSentinelGraph.Execute(targetSentinelCommands);
    EXPECT_EQ(0u, targetSentinelCommands.beginRenderPassCount);
    EXPECT_EQ(0u, targetSentinelCommands.drawIndexedCount);
    EXPECT_EQ(sourceSentinelIdentity, sourceSentinel.results->identity);
    EXPECT_EQ(sourceSentinelIdentity,
              sourceSentinel.results->directionalShadowOutput.identity);
    EXPECT_TRUE(sourceSentinel.results->directionalShadowOutput.enabled);
    EXPECT_EQ(779u, sourceSentinel.results->directionalShadowOutput.shadowMapSize);
    EXPECT_EQ(780u, sourceSentinel.results->objectVelocityStats.width);
    EXPECT_TRUE(sourceSentinel.results->objectVelocityStats.velocityRecorded);

    // A partially initialized foreign identity is still supplied ownership,
    // not permission for the helper to initialize/overwrite it.
    RenderGraph partialAttackerGraph;
    partialAttackerGraph.SetDevice(&device);
    auto partialForeignResults = std::make_shared<RenderPassRecordResults>();
    partialForeignResults->identity.graph = &foreignGraph;
    partialForeignResults->identity.graphIdentity = foreignGraph.GetGraphIdentity();
    partialForeignResults->identity.graphRecordingGeneration =
        foreignGraph.GetRecordingGeneration();
    partialForeignResults->objectVelocityStats.width = 778;
    partialForeignResults->objectVelocityStats.velocityRecorded = true;
    RenderPassRecordContext partialAttacker;
    partialAttacker.view.renderGraph = &partialAttackerGraph;
    partialAttacker.identity.graph = &partialAttackerGraph;
    partialAttacker.identity.graphIdentity = partialAttackerGraph.GetGraphIdentity();
    partialAttacker.identity.graphRecordingGeneration =
        partialAttackerGraph.GetRecordingGeneration();
    partialAttacker.identity.frameSequence = 622;
    partialAttacker.identity.recordEpoch = 622;
    partialAttacker.results = partialForeignResults;
    pass.AddToGraph(partialAttackerGraph, partialAttacker);
    partialAttackerGraph.Compile();
    ASSERT_TRUE(partialAttackerGraph.GetCompileStats().compileValid);
    RecordingCommandContext partialAttackerCommands;
    partialAttackerGraph.Execute(partialAttackerCommands);
    EXPECT_EQ(0u, partialForeignResults->identity.frameSequence);
    EXPECT_EQ(0u, partialForeignResults->identity.recordEpoch);
    EXPECT_EQ(778u, partialForeignResults->objectVelocityStats.width);
    EXPECT_TRUE(partialForeignResults->objectVelocityStats.velocityRecorded);
    EXPECT_EQ(0u, partialAttackerCommands.beginRenderPassCount);

    // Handles imported by a different graph are rejected before setup and can
    // neither issue a render pass nor declare the target graph's output.
    auto staleTargets = makeTargets(
        64, "ObjectVelocityStaleVelocity", "ObjectVelocityStaleDepth");
    ASSERT_TRUE(staleTargets.first);
    ASSERT_TRUE(staleTargets.second);
    RenderGraph sourceGraph;
    RenderGraph staleTargetGraph;
    sourceGraph.SetDevice(&device);
    staleTargetGraph.SetDevice(&device);
    const RGTextureHandle foreignVelocity = sourceGraph.ImportTexture(
        staleTargets.first.Get(), RHIResourceState::RenderTarget);
    const RGTextureHandle foreignDepth = sourceGraph.ImportTexture(
        staleTargets.second.Get(), RHIResourceState::DepthRead);
    RenderPassRecordContext stale = makeContext(
        staleTargetGraph, foreignVelocity, foreignDepth, 630, 630, true);
    pass.AddToGraph(staleTargetGraph, stale);
    staleTargetGraph.Compile();
    ASSERT_TRUE(staleTargetGraph.GetCompileStats().compileValid);
    RecordingCommandContext staleCommands;
    staleTargetGraph.Execute(staleCommands);
    EXPECT_EQ(0u, staleCommands.beginRenderPassCount);
    EXPECT_FALSE(stale.results->objectVelocityStats.outputDeclared);
    EXPECT_FALSE(stale.results->objectVelocityStats.velocityRecorded);

    // Provenance alone is insufficient: a forged handle can reproduce this
    // graph's identity and generation while referring past its resource table.
    // It must be rejected before resource retention or graph declarations.
    RenderGraph forgedGraph;
    forgedGraph.SetDevice(&device);
    RGTextureHandle forgedVelocity;
    forgedVelocity.index = 0;
    forgedVelocity.graphIdentity = forgedGraph.GetGraphIdentity();
    forgedVelocity.recordingGeneration = forgedGraph.GetRecordingGeneration();
    RGTextureHandle forgedDepth = forgedVelocity;
    forgedDepth.index = 1;
    RenderPassRecordContext forged = makeContext(
        forgedGraph, forgedVelocity, forgedDepth, 635, 635, true);
    pass.AddToGraph(forgedGraph, forged);
    forgedGraph.Compile();
    ASSERT_TRUE(forgedGraph.GetCompileStats().compileValid);
    const RenderGraph::Diagnostics forgedDiagnostics = forgedGraph.GetDiagnostics();
    const auto forgedPass = std::find_if(
        forgedDiagnostics.passes.begin(), forgedDiagnostics.passes.end(),
        [](const RenderGraph::PassDiagnostic& diagnostic)
        {
            return diagnostic.name == "ObjectVelocityPass";
        });
    ASSERT_NE(forgedDiagnostics.passes.end(), forgedPass);
    EXPECT_TRUE(forgedPass->usages.empty());
    RecordingCommandContext forgedCommands;
    forgedGraph.Execute(forgedCommands);
    EXPECT_EQ(0u, forgedCommands.beginRenderPassCount);
    EXPECT_EQ(0u, forgedCommands.drawIndexedCount);
    EXPECT_FALSE(forged.results->objectVelocityStats.outputDeclared);
    EXPECT_FALSE(forged.results->objectVelocityStats.velocityRecorded);

    // A batch that rejects retention is a recording failure before graph
    // accesses are declared, so no dangling ReadWrite/Read usage remains.
    RenderSubmissionTracker tracker;
    ASSERT_TRUE(tracker.Initialize(&device));
    RenderRetirementQueue retirement;
    ASSERT_TRUE(retirement.Initialize(&tracker));
    RenderSubmissionResourceBatch sealedBatch;
    sealedBatch.ReleaseUnsubmitted(retirement);
    ASSERT_TRUE(sealedBatch.IsSealed());
    RenderGraph rejectedGraph;
    rejectedGraph.SetDevice(&device);
    auto rejectedTargets = makeTargets(
        64, "ObjectVelocityRejectedVelocity", "ObjectVelocityRejectedDepth");
    ASSERT_TRUE(rejectedTargets.first);
    ASSERT_TRUE(rejectedTargets.second);
    RenderPassRecordContext rejected = makeContext(
        rejectedGraph,
        rejectedGraph.ImportTexture(rejectedTargets.first.Get(),
                                    RHIResourceState::RenderTarget),
        rejectedGraph.ImportTexture(rejectedTargets.second.Get(),
                                    RHIResourceState::DepthRead),
        640, 640, true, &sealedBatch);
    pass.AddToGraph(rejectedGraph, rejected);
    rejectedGraph.Compile();
    ASSERT_TRUE(rejectedGraph.GetCompileStats().compileValid);
    const RenderGraph::Diagnostics rejectedDiagnostics = rejectedGraph.GetDiagnostics();
    const auto rejectedPass = std::find_if(
        rejectedDiagnostics.passes.begin(), rejectedDiagnostics.passes.end(),
        [](const RenderGraph::PassDiagnostic& diagnostic)
        {
            return diagnostic.name == "ObjectVelocityPass";
        });
    ASSERT_NE(rejectedDiagnostics.passes.end(), rejectedPass);
    EXPECT_TRUE(rejectedPass->usages.empty());
    RecordingCommandContext rejectedCommands;
    rejectedGraph.Execute(rejectedCommands);
    EXPECT_EQ(0u, rejectedCommands.beginRenderPassCount);
    EXPECT_FALSE(rejected.results->objectVelocityStats.outputDeclared);
    EXPECT_FALSE(rejected.results->objectVelocityStats.velocityRecorded);
    EXPECT_EQ(GPUCompletionStatus::Completed, retirement.Poll());
    tracker.Shutdown();
}

TEST_F(RenderPassValidationFixture, DepthPrepassNoPlanDefaultDoesNotMutateTarget)
{
    RVX_REQUIRE_RENDER_RUNTIME_PIPELINE();

    std::vector<RenderDrawItem> opaqueItems = {
        MakeDrawItem(MaterialRenderMode::Opaque)};
    std::vector<RenderDrawItem> maskedItems;
    SceneMeshPassPreparation preparation = PrepareDepthPackets(
        opaqueItems, maskedItems);
    const RenderFramePlanCompileResult compiled = CompileForcedDirectPlan(
        preparation, 590);
    ASSERT_TRUE(compiled.succeeded);
    RenderFrameExecutionReport report = MakeExecutionReport(compiled.plan);
    RHITextureRef depthTexture = device.CreateTexture(
        RHITextureDesc::DepthStencil(64, 64, RHIFormat::D32_FLOAT));
    RHITextureViewRef depthView = device.CreateTextureView(depthTexture.Get());
    ASSERT_TRUE(depthView);

    DepthPrepass pass;
    pass.SetEnabled(true);
    ConfigureResources(pass, gpuResources, pipelineCache);

    RenderGraph graph;
    graph.SetDevice(&device);
    ViewData malformedView = view;
    malformedView.depthTarget = graph.ImportTexture(
        depthTexture.Get(), RHIResourceState::DepthWrite);
    RenderPassRecordContext malformed = MakeMainSceneRecordContext(
        graph, malformedView, scene, opaqueItems, maskedItems, compiled.plan,
        preparation, report, compiled.plan.frameSequence);
    malformed.executionPlan = nullptr;
    malformed.view.renderFrameExecutionPlan = nullptr;
    pass.AddToGraph(graph, malformed);
    graph.Compile();
    RecordingCommandContext ctx;
    graph.Execute(ctx);

    EXPECT_EQ(0u, ctx.beginRenderPassCount);
    EXPECT_EQ(0u, ctx.endRenderPassCount);
    EXPECT_EQ(0u, ctx.drawIndexedCount);
    EXPECT_EQ(0u, ctx.drawIndexedIndirectCount);
    EXPECT_FALSE(pass.GetDrawStats().gpuDrivenRequested);
}

TEST_F(RenderPassValidationFixture, OpaquePassNoPlanDefaultDoesNotMutateTarget)
{
    RVX_REQUIRE_RENDER_RUNTIME_PIPELINE();

    std::vector<RenderDrawItem> opaqueItems = {
        MakeDrawItem(MaterialRenderMode::Opaque)};
    std::vector<RenderDrawItem> maskedItems;
    SceneMeshPassPreparation preparation = PrepareOpaquePackets(
        opaqueItems, maskedItems);
    const RenderFramePlanCompileResult compiled = CompileForcedDirectPlan(
        preparation, 591);
    ASSERT_TRUE(compiled.succeeded);
    RenderFrameExecutionReport report = MakeExecutionReport(compiled.plan);

    OpaquePass pass;
    ConfigureResources(pass, gpuResources, pipelineCache, materialSystem);

    RenderGraph graph;
    graph.SetDevice(&device);
    ViewData malformedView = view;
    malformedView.colorTarget = graph.ImportTexture(
        colorView->GetTexture(), RHIResourceState::RenderTarget);
    RenderPassRecordContext malformed = MakeMainSceneRecordContext(
        graph, malformedView, scene, opaqueItems, maskedItems, compiled.plan,
        preparation, report, compiled.plan.frameSequence);
    malformed.executionPlan = nullptr;
    malformed.view.renderFrameExecutionPlan = nullptr;
    pass.AddToGraph(graph, malformed);
    graph.Compile();
    RecordingCommandContext ctx;
    graph.Execute(ctx);

    EXPECT_EQ(0u, ctx.beginRenderPassCount);
    EXPECT_EQ(0u, ctx.endRenderPassCount);
    EXPECT_EQ(0u, ctx.drawIndexedCount);
    EXPECT_EQ(0u, ctx.drawIndexedIndirectCount);
    EXPECT_FALSE(pass.GetDrawStats().gpuDrivenRequested);
}

TEST_F(RenderPassValidationFixture, DepthPrepassConsumesGPUDrivenMultiMeshIndirectStreams)
{
    RVX_REQUIRE_RENDER_RUNTIME_PIPELINE();

    auto secondMeshResource = CreateMeshResource(402);
    gpuResources.UploadImmediate(secondMeshResource.get());
    ASSERT_TRUE(gpuResources.IsGPUReady(secondMeshResource->GetId()));

    scene.GetMutableObject(0).bounds = meshResource->GetBounds();
    RenderObject secondObject = MakeRenderObject(*secondMeshResource, gpuResources);
    secondObject.bounds = secondMeshResource->GetBounds();
    scene.AddObject(secondObject);

    RenderDrawItem firstItem = MakeDrawItem(MaterialRenderMode::Opaque);
    RenderDrawItem secondItem = firstItem;
    secondItem.objectIndex = 1;
    secondItem.mesh = gpuResources.GetHandle(secondMeshResource->GetId());
    std::vector<RenderDrawItem> opaqueItems = {firstItem, secondItem};
    std::vector<RenderDrawItem> maskedItems;

    MeshGPUBuffers firstBuffers = gpuResources.GetMeshBuffers(meshResource->GetId());
    ASSERT_TRUE(firstBuffers.IsValid());
    ASSERT_FALSE(firstBuffers.submeshes.empty());
    MeshGPUBuffers secondBuffers = gpuResources.GetMeshBuffers(secondMeshResource->GetId());
    ASSERT_TRUE(secondBuffers.IsValid());
    ASSERT_FALSE(secondBuffers.submeshes.empty());

    opaqueItems[0].packet = MakeDepthPacket(
        scene, 0, 0, firstBuffers, opaqueItems[0].material,
        RenderMaterialMode::Opaque, RenderDrawFlags::None);
    opaqueItems[1].packet = MakeDepthPacket(
        scene, 1, 0, secondBuffers, opaqueItems[1].material,
        RenderMaterialMode::Opaque, RenderDrawFlags::None);
    SceneMeshPassPreparation preparation = PrepareDepthPackets(
        opaqueItems, maskedItems);
    const RenderFramePlanCompileResult compiled = CompileForcedGPUPlan(
        preparation, 592);
    ASSERT_TRUE(compiled.succeeded);
    RenderFrameExecutionReport report = MakeExecutionReport(compiled.plan);

    GPUCulling culling;
    GPUCullingConfig cullingConfig;
    cullingConfig.maxInstances = 4;
    cullingConfig.enableOcclusionCulling = false;
    cullingConfig.enableDistanceCulling = false;
    culling.Initialize(&device, cullingConfig);
    culling.BeginFrame();

    GPUIndexedDrawDesc firstDrawDesc;
    firstDrawDesc.indexCount = firstBuffers.submeshes[0].indexCount;
    firstDrawDesc.firstIndex = firstBuffers.submeshes[0].indexOffset;
    firstDrawDesc.vertexOffset = firstBuffers.submeshes[0].baseVertex;
    ASSERT_EQ(0u, culling.BeginDrawGroup(meshResource->GetId()));
    EXPECT_NE(RVX_INVALID_INDEX, culling.AddDrawItemInstance(scene, firstItem, firstDrawDesc, 0));
    culling.EndDrawGroup();

    GPUIndexedDrawDesc secondDrawDesc;
    secondDrawDesc.indexCount = secondBuffers.submeshes[0].indexCount;
    secondDrawDesc.firstIndex = secondBuffers.submeshes[0].indexOffset;
    secondDrawDesc.vertexOffset = secondBuffers.submeshes[0].baseVertex;
    ASSERT_EQ(1u, culling.BeginDrawGroup(secondMeshResource->GetId()));
    EXPECT_NE(RVX_INVALID_INDEX, culling.AddDrawItemInstance(scene, secondItem, secondDrawDesc, 1));
    culling.EndDrawGroup();
    culling.EndFrame();
    culling.CullCpuFallback(view.viewMatrix, view.projectionMatrix);
    EXPECT_EQ(2u, culling.GetDrawCount());
    ASSERT_EQ(2u, culling.GetDrawGroups().size());

    RHITextureRef depthTexture = device.CreateTexture(RHITextureDesc::DepthStencil(64, 64, RHIFormat::D32_FLOAT));
    ASSERT_TRUE(depthTexture);
    RHITextureViewRef depthView = device.CreateTextureView(depthTexture.Get());
    ASSERT_TRUE(depthView);

    DepthPrepass pass;
    pass.SetEnabled(true);
    ConfigureResources(pass, gpuResources, pipelineCache);

    RecordingCommandContext ctx;
    (void)ExecuteTypedDepthRecording(
        pass, view, opaqueItems, maskedItems, compiled.plan, preparation,
        report, depthView.Get(), &culling, ctx);

    EXPECT_EQ(1u, ctx.beginRenderPassCount);
    EXPECT_EQ(1u, ctx.endRenderPassCount);
    EXPECT_EQ(0u, ctx.drawIndexedCount);
    EXPECT_EQ(2u, ctx.drawIndexedIndirectCount);
    EXPECT_EQ(1u, ctx.lastIndirectDrawCount);
    EXPECT_EQ(sizeof(IndirectDrawIndexedCommand), ctx.lastIndirectOffset);
    EXPECT_EQ(sizeof(IndirectDrawIndexedCommand), ctx.lastIndirectStride);
    ASSERT_NE(ctx.currentPipeline, nullptr);
    ExpectPipelineDebugName(ctx.currentPipeline, "GPUDrivenDepthOnlyPipeline");

    const DepthPrepassDrawStats& stats = pass.GetDrawStats();
    EXPECT_TRUE(stats.gpuDrivenRequested);
    EXPECT_TRUE(stats.gpuDrivenEligible);
    EXPECT_EQ(0u, stats.directDrawCount);
    EXPECT_EQ(2u, stats.gpuDrivenIndirectBatchCount);
    EXPECT_EQ(2u, stats.gpuDrivenIndirectDrawCount);
}

TEST_F(RenderPassValidationFixture,
       DepthPrepassRejectsCorruptPublishedGPUPlanBeforeRecording)
{
    RVX_REQUIRE_RENDER_RUNTIME_PIPELINE();

    scene.GetMutableObject(0).entityId = 69;
    const MeshGPUBuffers buffers =
        gpuResources.GetMeshBuffers(meshResource->GetId());
    ASSERT_TRUE(buffers.IsValid());
    RenderDrawItem item = MakeDrawItem(MaterialRenderMode::Opaque);
    item.packet = MakeDepthPacket(
        scene, 0, 0, buffers, item.material,
        RenderMaterialMode::Opaque, RenderDrawFlags::None);
    std::vector<RenderDrawItem> opaqueItems = {item};
    std::vector<RenderDrawItem> maskedItems;
    SceneMeshPassPreparation preparation = PrepareDepthPackets(
        opaqueItems, maskedItems);
    RenderFramePlanCompileResult compiled =
        CompileForcedGPUPlan(preparation, 599);
    ASSERT_TRUE(compiled.succeeded);
    const auto depthPlan = std::find_if(
        compiled.plan.passes.begin(),
        compiled.plan.passes.end(),
        [](const RenderPassExecutionPlan& passPlan)
        {
            return passPlan.pass == RenderPassKind::Depth;
        });
    ASSERT_NE(depthPlan, compiled.plan.passes.end());
    ASSERT_GT(depthPlan->gpuEligiblePackets.count, 0u);
    compiled.plan.packetReferences[
        depthPlan->gpuEligiblePackets.first].sourceOrdinal++;
    RenderFrameExecutionReport report = MakeExecutionReport(compiled.plan);

    GPUCulling culling;
    GPUCullingConfig cullingConfig;
    cullingConfig.maxInstances = 1;
    cullingConfig.enableOcclusionCulling = false;
    cullingConfig.enableDistanceCulling = false;
    culling.Initialize(&device, cullingConfig);
    culling.BeginFrame();
    GPUIndexedDrawDesc drawDesc;
    drawDesc.indexCount = buffers.submeshes[0].indexCount;
    drawDesc.firstIndex = buffers.submeshes[0].indexOffset;
    drawDesc.vertexOffset = buffers.submeshes[0].baseVertex;
    ASSERT_EQ(0u, culling.BeginDrawGroup(meshResource->GetId()));
    EXPECT_NE(RVX_INVALID_INDEX,
              culling.AddDrawItemInstance(scene, item, drawDesc, 0));
    culling.EndDrawGroup();
    culling.EndFrame();
    culling.CullCpuFallback(view.viewMatrix, view.projectionMatrix);

    RHITextureRef depthTexture = device.CreateTexture(
        RHITextureDesc::DepthStencil(64, 64, RHIFormat::D32_FLOAT));
    RHITextureViewRef depthView = device.CreateTextureView(depthTexture.Get());
    ASSERT_TRUE(depthView);
    view.viewCache = &viewCache;
    view.renderFrameExecutionPlan = &compiled.plan;
    view.meshPassPreparation = &preparation;
    view.renderFrameExecutionReport = &report;

    DepthPrepass pass;
    pass.SetEnabled(true);
    ConfigureResources(pass, gpuResources, pipelineCache);
    RecordingCommandContext ctx;
    (void)ExecuteTypedDepthRecording(
        pass, view, opaqueItems, maskedItems, compiled.plan, preparation,
        report, depthView.Get(), &culling, ctx);

    EXPECT_EQ(0u, ctx.beginRenderPassCount);
    EXPECT_EQ(0u, ctx.drawIndexedCount);
    EXPECT_EQ(0u, ctx.drawIndexedIndirectCount);
    EXPECT_EQ(RenderPolicyReason::InconsistentFacts,
              pass.GetDrawStats().failureReason);
    ASSERT_FALSE(report.passes.empty());
    EXPECT_EQ(RenderExecutionStatus::Failed,
              report.passes.front().gpuDrivenLane.status);
}

TEST_F(RenderPassValidationFixture,
       OpaquePassRejectsStaleGPUPreparationBeforeRecording)
{
    RVX_REQUIRE_RENDER_RUNTIME_PIPELINE();

    scene.GetMutableObject(0).entityId = 70;
    const MeshGPUBuffers buffers =
        gpuResources.GetMeshBuffers(meshResource->GetId());
    ASSERT_TRUE(buffers.IsValid());
    RenderDrawItem item = MakeDrawItem(MaterialRenderMode::Opaque);
    item.packet = MakeOpaquePacket(
        scene, 0, 0, buffers, item.material,
        RenderMaterialMode::Opaque, RenderDrawFlags::None);
    std::vector<RenderDrawItem> opaqueItems = {item};
    std::vector<RenderDrawItem> maskedItems;
    SceneMeshPassPreparation preparation = PrepareOpaquePackets(
        opaqueItems, maskedItems);
    const RenderFramePlanCompileResult compiled =
        CompileForcedGPUPlan(preparation, 600);
    ASSERT_TRUE(compiled.succeeded);
    preparation.opaque.packets.front().sourceOrdinal++;
    RenderFrameExecutionReport report = MakeExecutionReport(compiled.plan);

    GPUCulling culling;
    GPUCullingConfig cullingConfig;
    cullingConfig.maxInstances = 1;
    cullingConfig.enableOcclusionCulling = false;
    cullingConfig.enableDistanceCulling = false;
    culling.Initialize(&device, cullingConfig);
    culling.BeginFrame();
    GPUIndexedDrawDesc drawDesc;
    drawDesc.indexCount = buffers.submeshes[0].indexCount;
    drawDesc.firstIndex = buffers.submeshes[0].indexOffset;
    drawDesc.vertexOffset = buffers.submeshes[0].baseVertex;
    ASSERT_EQ(0u, culling.BeginDrawGroup(meshResource->GetId()));
    EXPECT_NE(RVX_INVALID_INDEX,
              culling.AddDrawItemInstance(scene, item, drawDesc, 0));
    culling.EndDrawGroup();
    culling.EndFrame();
    culling.CullCpuFallback(view.viewMatrix, view.projectionMatrix);
    view.viewCache = &viewCache;
    view.renderFrameExecutionPlan = &compiled.plan;
    view.meshPassPreparation = &preparation;
    view.renderFrameExecutionReport = &report;

    OpaquePass pass;
    ConfigureResources(pass, gpuResources, pipelineCache, materialSystem);
    RecordingCommandContext ctx;
    (void)ExecuteTypedOpaqueRecording(
        pass, view, opaqueItems, maskedItems, compiled.plan, preparation,
        report, colorView.Get(), nullptr, &culling, ctx);

    EXPECT_EQ(0u, ctx.beginRenderPassCount);
    EXPECT_EQ(0u, ctx.drawIndexedCount);
    EXPECT_EQ(0u, ctx.drawIndexedIndirectCount);
    EXPECT_EQ(RenderPolicyReason::InconsistentFacts,
              pass.GetDrawStats().failureReason);
    const RenderPassExecutionReport* opaqueReport =
        FindPassExecutionReport(report, RenderPassKind::Opaque);
    ASSERT_NE(opaqueReport, nullptr);
    EXPECT_EQ(RenderExecutionStatus::Failed,
              opaqueReport->gpuDrivenLane.status);
}

TEST_F(RenderPassValidationFixture,
       DepthPrepassRecordsPlannedStaticMaskedAndDefaultMaterialPackets)
{
    RVX_REQUIRE_RENDER_RUNTIME_PIPELINE();

    scene.GetMutableObject(0).entityId = 71;
    const MeshGPUBuffers buffers =
        gpuResources.GetMeshBuffers(meshResource->GetId());
    ASSERT_TRUE(buffers.IsValid());
    ASSERT_FALSE(buffers.submeshes.empty());

    RenderDrawItem opaqueItem = MakeDrawItem(MaterialRenderMode::Opaque);
    opaqueItem.packet = MakeDepthPacket(
        scene, 0, 0, buffers, opaqueItem.material,
        RenderMaterialMode::Opaque, RenderDrawFlags::None);
    RenderDrawItem maskedItem = MakeDrawItem(MaterialRenderMode::Masked);
    maskedItem.material = {};
    maskedItem.packet = MakeDepthPacket(
        scene, 0, 0, buffers, {}, RenderMaterialMode::Masked,
        RenderDrawFlags::Masked | RenderDrawFlags::MissingMaterial);
    std::vector<RenderDrawItem> opaqueItems = {opaqueItem};
    std::vector<RenderDrawItem> maskedItems = {maskedItem};

    SceneMeshPassPreparation preparation = PrepareDepthPackets(
        opaqueItems, maskedItems);
    const RenderFramePlanCompileResult compiled =
        CompileForcedDirectPlan(preparation);
    ASSERT_TRUE(compiled.succeeded);
    RenderFrameExecutionReport report = MakeExecutionReport(compiled.plan);

    RHITextureRef depthTexture = device.CreateTexture(
        RHITextureDesc::DepthStencil(64, 64, RHIFormat::D32_FLOAT));
    ASSERT_TRUE(depthTexture);
    RHITextureViewRef depthView = device.CreateTextureView(depthTexture.Get());
    ASSERT_TRUE(depthView);

    view.viewCache = &viewCache;
    view.renderFrameExecutionPlan = &compiled.plan;
    view.meshPassPreparation = &preparation;
    view.renderFrameExecutionReport = &report;
    view.viewportWidth = 64;
    view.viewportHeight = 64;

    DepthPrepass pass;
    pass.SetEnabled(true);
    ConfigureResources(pass, gpuResources, pipelineCache);
    pass.SetMaterialSystem(&materialSystem);

    RecordingCommandContext ctx;
    ExecuteTypedDepthRecording(
        pass, view, opaqueItems, maskedItems, compiled.plan, preparation,
        report, depthView.Get(), nullptr, ctx);

    EXPECT_EQ(1u, ctx.beginRenderPassCount);
    EXPECT_EQ(1u, ctx.endRenderPassCount);
    EXPECT_EQ(2u, ctx.drawIndexedCount);
    ASSERT_EQ(2u, ctx.pipelineSequence.size());
    EXPECT_EQ(pipelineCache.GetDepthOnlyPipeline(), ctx.pipelineSequence[0]);
    EXPECT_EQ(pipelineCache.GetMaskedDepthOnlyPipeline(),
              ctx.pipelineSequence[1]);
    EXPECT_TRUE(std::any_of(ctx.descriptorSetSequence.begin(),
                            ctx.descriptorSetSequence.end(),
                            [](uint32 set) { return set == 2; }));

    const DepthPrepassDrawStats& stats = pass.GetDrawStats();
    EXPECT_TRUE(stats.planRequested);
    EXPECT_TRUE(stats.planValidated);
    EXPECT_TRUE(stats.directPacketPathUsed);
    EXPECT_EQ(2u, stats.plannedPacketCount);
    EXPECT_EQ(2u, stats.executedPacketCount);

    ASSERT_FALSE(report.passes.empty());
    const RenderPassExecutionReport& depthReport = report.passes.front();
    EXPECT_EQ(RenderExecutionStatus::Completed, depthReport.status);
    EXPECT_EQ(RenderExecutionStatus::Completed,
              depthReport.directLane.status);
    EXPECT_EQ(RenderExecutionStatus::NotAttempted,
              depthReport.gpuDrivenLane.status);
    EXPECT_EQ(2u, depthReport.directLane.executedPacketCount);
}

TEST_F(RenderPassValidationFixture,
       DepthPrepassRecordsPlannedSkinnedPacket)
{
    RVX_REQUIRE_RENDER_RUNTIME_PIPELINE();

    auto skinnedMesh = CreateTwoSubmeshMeshResource(402);
    skinnedMesh->GetMesh()->SetBoneData(
        std::vector<IVec4>(6, IVec4(0, 0, 0, 0)),
        std::vector<Vec4>(6, Vec4(1.0f, 0.0f, 0.0f, 0.0f)));
    ASSERT_TRUE(gpuResources.UploadImmediate(skinnedMesh.get()));
    const MeshGPUBuffers buffers =
        gpuResources.GetMeshBuffers(skinnedMesh->GetId());
    ASSERT_TRUE(buffers.IsValid());
    ASSERT_TRUE(buffers.HasSkinningVertexData());

    RenderObject object = MakeRenderObject(*skinnedMesh, gpuResources);
    object.entityId = 72;
    object.skinningMatrices.push_back(Mat4Identity());
    scene.AddObject(object);
    RenderDrawItem item;
    item.objectIndex = 1;
    item.submeshIndex = 1;
    item.mesh = object.mesh;
    item.renderMode = RenderMaterialMode::Opaque;
    item.packet = MakeDepthPacket(
        scene, 1, 1, buffers, {}, RenderMaterialMode::Opaque,
        RenderDrawFlags::Skinned | RenderDrawFlags::MissingMaterial);
    std::vector<RenderDrawItem> opaqueItems = {item};
    std::vector<RenderDrawItem> maskedItems;

    SceneMeshPassPreparation preparation = PrepareDepthPackets(
        opaqueItems, maskedItems);
    const RenderFramePlanCompileResult compiled =
        CompileForcedDirectPlan(preparation, 602);
    ASSERT_TRUE(compiled.succeeded);
    RenderFrameExecutionReport report = MakeExecutionReport(compiled.plan);

    RHITextureRef depthTexture = device.CreateTexture(
        RHITextureDesc::DepthStencil(64, 64, RHIFormat::D32_FLOAT));
    RHITextureViewRef depthView = device.CreateTextureView(depthTexture.Get());
    ASSERT_TRUE(depthView);
    view.viewCache = &viewCache;
    view.renderFrameExecutionPlan = &compiled.plan;
    view.meshPassPreparation = &preparation;
    view.renderFrameExecutionReport = &report;

    DepthPrepass pass;
    pass.SetEnabled(true);
    ConfigureResources(pass, gpuResources, pipelineCache);
    pass.SetMaterialSystem(&materialSystem);

    RecordingCommandContext ctx;
    ExecuteTypedDepthRecording(
        pass, view, opaqueItems, maskedItems, compiled.plan, preparation,
        report, depthView.Get(), nullptr, ctx);

    EXPECT_EQ(1u, ctx.beginRenderPassCount);
    EXPECT_EQ(1u, ctx.drawIndexedCount);
    EXPECT_TRUE(pass.GetDrawStats().planValidated);
    EXPECT_EQ(1u, pass.GetDrawStats().executedPacketCount);
}

TEST_F(RenderPassValidationFixture,
       DepthPrepassRecordsMixedPublishedGPUAndDirectLanesInSingleRenderPass)
{
    RVX_REQUIRE_RENDER_RUNTIME_PIPELINE();

    scene.GetMutableObject(0).entityId = 77;
    scene.GetMutableObject(0).bounds = meshResource->GetBounds();
    const MeshGPUBuffers staticBuffers =
        gpuResources.GetMeshBuffers(meshResource->GetId());
    ASSERT_TRUE(staticBuffers.IsValid());
    ASSERT_FALSE(staticBuffers.submeshes.empty());

    auto skinnedMesh = CreateTwoSubmeshMeshResource(421);
    skinnedMesh->GetMesh()->SetBoneData(
        std::vector<IVec4>(6, IVec4(0, 0, 0, 0)),
        std::vector<Vec4>(6, Vec4(1.0f, 0.0f, 0.0f, 0.0f)));
    ASSERT_TRUE(gpuResources.UploadImmediate(skinnedMesh.get()));
    const MeshGPUBuffers skinnedBuffers =
        gpuResources.GetMeshBuffers(skinnedMesh->GetId());
    ASSERT_TRUE(skinnedBuffers.IsValid());
    ASSERT_TRUE(skinnedBuffers.HasSkinningVertexData());
    ASSERT_GE(skinnedBuffers.submeshes.size(), 2u);

    RenderObject skinnedObject =
        MakeRenderObject(*skinnedMesh, gpuResources);
    skinnedObject.entityId = 78;
    skinnedObject.bounds = skinnedMesh->GetBounds();
    skinnedObject.skinningMatrices.push_back(Mat4Identity());
    scene.AddObject(skinnedObject);

    RenderDrawItem gpuCandidate = MakeDrawItem(MaterialRenderMode::Opaque);
    gpuCandidate.packet = MakeDepthPacket(
        scene, 0, 0, staticBuffers, gpuCandidate.material,
        RenderMaterialMode::Opaque, RenderDrawFlags::None);
    RenderDrawItem directItem = MakeDrawItem(MaterialRenderMode::Opaque);
    directItem.objectIndex = 1;
    directItem.submeshIndex = 1;
    directItem.mesh = skinnedObject.mesh;
    directItem.packet = MakeDepthPacket(
        scene, 1, 1, skinnedBuffers, directItem.material,
        RenderMaterialMode::Opaque, RenderDrawFlags::Skinned);
    std::vector<RenderDrawItem> opaqueItems = {gpuCandidate, directItem};
    std::vector<RenderDrawItem> maskedItems;

    SceneMeshPassPreparation preparation = PrepareDepthPackets(
        opaqueItems, maskedItems);
    const RenderFramePlanCompileResult compiled =
        CompileForcedGPUPlan(preparation, 608);
    ASSERT_TRUE(compiled.succeeded);
    const auto depthPlan = std::find_if(
        compiled.plan.passes.begin(),
        compiled.plan.passes.end(),
        [](const RenderPassExecutionPlan& passPlan)
        {
            return passPlan.pass == RenderPassKind::Depth;
        });
    ASSERT_NE(depthPlan, compiled.plan.passes.end());
    EXPECT_EQ(1u, depthPlan->partition.gpuDrivenPacketCount);
    EXPECT_EQ(1u, depthPlan->partition.directPacketCount);
    EXPECT_EQ(0u, depthPlan->partition.skippedPacketCount);
    EXPECT_EQ(1u, depthPlan->partition.drawGroupCount);
    EXPECT_EQ(2u, depthPlan->identityAccounting.expectedPacketCount);
    EXPECT_TRUE(depthPlan->identityAccounting.IsExactlyOnce());
    RenderFrameExecutionReport report = MakeExecutionReport(compiled.plan);

    GPUCulling culling;
    GPUCullingConfig cullingConfig;
    cullingConfig.maxInstances = 4;
    cullingConfig.enableOcclusionCulling = false;
    cullingConfig.enableDistanceCulling = false;
    culling.Initialize(&device, cullingConfig);
    culling.BeginFrame();
    GPUIndexedDrawDesc drawDesc;
    drawDesc.indexCount = staticBuffers.submeshes[0].indexCount;
    drawDesc.firstIndex = staticBuffers.submeshes[0].indexOffset;
    drawDesc.vertexOffset = staticBuffers.submeshes[0].baseVertex;
    ASSERT_EQ(0u, culling.BeginDrawGroup(meshResource->GetId()));
    EXPECT_NE(RVX_INVALID_INDEX,
              culling.AddDrawItemInstance(scene, gpuCandidate, drawDesc, 0));
    culling.EndDrawGroup();
    culling.EndFrame();
    culling.CullCpuFallback(view.viewMatrix, view.projectionMatrix);
    ASSERT_EQ(1u, culling.GetDrawGroups().size());
    EXPECT_EQ(1u, culling.GetDrawGroups()[0].maxDrawCount);
    EXPECT_EQ(1u, culling.GetInstanceCount());

    RHITextureRef depthTexture = device.CreateTexture(
        RHITextureDesc::DepthStencil(64, 64, RHIFormat::D32_FLOAT));
    RHITextureViewRef depthView = device.CreateTextureView(depthTexture.Get());
    ASSERT_TRUE(depthView);
    view.viewCache = &viewCache;
    view.renderFrameExecutionPlan = &compiled.plan;
    view.meshPassPreparation = &preparation;
    view.renderFrameExecutionReport = &report;

    DepthPrepass pass;
    pass.SetEnabled(true);
    ConfigureResources(pass, gpuResources, pipelineCache);
    pass.SetMaterialSystem(&materialSystem);
    RecordingCommandContext ctx;
    ExecuteTypedDepthRecording(
        pass, view, opaqueItems, maskedItems, compiled.plan, preparation,
        report, depthView.Get(), &culling, ctx);

    EXPECT_EQ(1u, ctx.beginRenderPassCount);
    EXPECT_EQ(1u, ctx.endRenderPassCount);
    ASSERT_EQ(1u, ctx.renderPasses.size());
    EXPECT_TRUE(ctx.renderPasses[0].hasDepthStencil);
    EXPECT_EQ(RHILoadOp::Clear,
              ctx.renderPasses[0].depthStencilAttachment.depthLoadOp);
    EXPECT_EQ(1u, ctx.drawIndexedIndirectCount);
    EXPECT_EQ(1u, ctx.drawIndexedCount);

    const DepthPrepassDrawStats& stats = pass.GetDrawStats();
    EXPECT_TRUE(stats.planRequested);
    EXPECT_TRUE(stats.planValidated);
    EXPECT_EQ(2u, stats.plannedPacketCount);
    EXPECT_EQ(1u, stats.gpuDrivenIndirectDrawCount);
    EXPECT_EQ(1u, stats.directDrawCount);
    EXPECT_EQ(1u, stats.executedPacketCount);
    EXPECT_EQ(RenderPolicyReason::None, stats.failureReason);

    const RenderPassExecutionReport* depthReport =
        FindPassExecutionReport(report, RenderPassKind::Depth);
    ASSERT_NE(depthReport, nullptr);
    EXPECT_EQ(RenderExecutionStatus::Completed, depthReport->status);
    EXPECT_EQ(RenderExecutionStatus::Completed,
              depthReport->gpuDrivenLane.status);
    EXPECT_EQ(RenderExecutionStatus::Completed,
              depthReport->directLane.status);
    EXPECT_EQ(1u, depthReport->gpuDrivenLane.executedPacketCount);
    EXPECT_EQ(1u, depthReport->directLane.executedPacketCount);
}

TEST_F(RenderPassValidationFixture,
       DepthPrepassKeepsPreflightedDirectLaneAfterMixedGPULaneFailure)
{
    RVX_REQUIRE_RENDER_RUNTIME_PIPELINE();

    scene.GetMutableObject(0).entityId = 79;
    scene.GetMutableObject(0).bounds = meshResource->GetBounds();
    const MeshGPUBuffers staticBuffers =
        gpuResources.GetMeshBuffers(meshResource->GetId());
    ASSERT_TRUE(staticBuffers.IsValid());
    ASSERT_FALSE(staticBuffers.submeshes.empty());

    auto skinnedMesh = CreateTwoSubmeshMeshResource(422);
    skinnedMesh->GetMesh()->SetBoneData(
        std::vector<IVec4>(6, IVec4(0, 0, 0, 0)),
        std::vector<Vec4>(6, Vec4(1.0f, 0.0f, 0.0f, 0.0f)));
    ASSERT_TRUE(gpuResources.UploadImmediate(skinnedMesh.get()));
    const MeshGPUBuffers skinnedBuffers =
        gpuResources.GetMeshBuffers(skinnedMesh->GetId());
    ASSERT_TRUE(skinnedBuffers.IsValid());
    ASSERT_TRUE(skinnedBuffers.HasSkinningVertexData());
    ASSERT_GE(skinnedBuffers.submeshes.size(), 2u);

    RenderObject skinnedObject =
        MakeRenderObject(*skinnedMesh, gpuResources);
    skinnedObject.entityId = 80;
    skinnedObject.bounds = skinnedMesh->GetBounds();
    skinnedObject.skinningMatrices.push_back(Mat4Identity());
    scene.AddObject(skinnedObject);

    RenderDrawItem gpuCandidate = MakeDrawItem(MaterialRenderMode::Opaque);
    gpuCandidate.packet = MakeDepthPacket(
        scene, 0, 0, staticBuffers, gpuCandidate.material,
        RenderMaterialMode::Opaque, RenderDrawFlags::None);
    RenderDrawItem directItem = MakeDrawItem(MaterialRenderMode::Opaque);
    directItem.objectIndex = 1;
    directItem.submeshIndex = 1;
    directItem.mesh = skinnedObject.mesh;
    directItem.packet = MakeDepthPacket(
        scene, 1, 1, skinnedBuffers, directItem.material,
        RenderMaterialMode::Opaque, RenderDrawFlags::Skinned);
    std::vector<RenderDrawItem> opaqueItems = {gpuCandidate, directItem};
    std::vector<RenderDrawItem> maskedItems;
    SceneMeshPassPreparation preparation = PrepareDepthPackets(
        opaqueItems, maskedItems);
    const RenderFramePlanCompileResult compiled =
        CompileForcedGPUPlan(preparation, 609);
    ASSERT_TRUE(compiled.succeeded);
    RenderFrameExecutionReport report = MakeExecutionReport(compiled.plan);

    GPUCulling culling;
    GPUCullingConfig cullingConfig;
    cullingConfig.maxInstances = 4;
    cullingConfig.enableOcclusionCulling = false;
    cullingConfig.enableDistanceCulling = false;
    culling.Initialize(&device, cullingConfig);
    culling.BeginFrame();
    GPUIndexedDrawDesc drawDesc;
    drawDesc.indexCount = staticBuffers.submeshes[0].indexCount;
    drawDesc.firstIndex = staticBuffers.submeshes[0].indexOffset;
    drawDesc.vertexOffset = staticBuffers.submeshes[0].baseVertex;
    ASSERT_EQ(0u, culling.BeginDrawGroup(meshResource->GetId()));
    EXPECT_NE(RVX_INVALID_INDEX,
              culling.AddDrawItemInstance(scene, gpuCandidate, drawDesc, 0));
    culling.EndDrawGroup();
    culling.EndFrame();
    culling.CullCpuFallback(view.viewMatrix, view.projectionMatrix);

    RHITextureRef depthTexture = device.CreateTexture(
        RHITextureDesc::DepthStencil(64, 64, RHIFormat::D32_FLOAT));
    RHITextureViewRef depthView = device.CreateTextureView(depthTexture.Get());
    ASSERT_TRUE(depthView);
    view.viewCache = &viewCache;
    view.renderFrameExecutionPlan = &compiled.plan;
    view.meshPassPreparation = &preparation;
    view.renderFrameExecutionReport = &report;

    // The Direct batch is prepared before attachment mutation.  Fail only the
    // late GPU pipeline creation to prove its preflighted packet still records
    // once, without replaying the GPU candidate through Direct.
    device.failGraphicsPipelineDebugName = "GPUDrivenDepthOnlyPipeline";
    DepthPrepass pass;
    pass.SetEnabled(true);
    ConfigureResources(pass, gpuResources, pipelineCache);
    pass.SetMaterialSystem(&materialSystem);
    RecordingCommandContext ctx;
    ExecuteTypedDepthRecording(
        pass, view, opaqueItems, maskedItems, compiled.plan, preparation,
        report, depthView.Get(), &culling, ctx);

    EXPECT_EQ(1u, ctx.beginRenderPassCount);
    EXPECT_EQ(1u, ctx.endRenderPassCount);
    EXPECT_EQ(0u, ctx.drawIndexedIndirectCount);
    EXPECT_EQ(1u, ctx.drawIndexedCount);
    EXPECT_EQ(1u, pass.GetDrawStats().directDrawCount);
    EXPECT_EQ(0u, pass.GetDrawStats().gpuDrivenIndirectDrawCount);
    EXPECT_EQ(RenderPolicyReason::UnexpectedRecordingFailure,
              pass.GetDrawStats().failureReason);

    const RenderPassExecutionReport* depthReport =
        FindPassExecutionReport(report, RenderPassKind::Depth);
    ASSERT_NE(depthReport, nullptr);
    EXPECT_EQ(RenderExecutionStatus::Failed, depthReport->status);
    EXPECT_EQ(RenderExecutionStatus::Failed,
              depthReport->gpuDrivenLane.status);
    EXPECT_EQ(RenderExecutionStatus::Completed,
              depthReport->directLane.status);
    EXPECT_EQ(0u, depthReport->gpuDrivenLane.executedPacketCount);
    EXPECT_EQ(1u, depthReport->directLane.executedPacketCount);
}

TEST_F(RenderPassValidationFixture,
       DepthPrepassRejectsInvalidSubmeshBeforeRecording)
{
    RVX_REQUIRE_RENDER_RUNTIME_PIPELINE();

    scene.GetMutableObject(0).entityId = 73;
    const MeshGPUBuffers buffers =
        gpuResources.GetMeshBuffers(meshResource->GetId());
    ASSERT_TRUE(buffers.IsValid());
    RenderDrawItem item = MakeDrawItem(MaterialRenderMode::Opaque);
    item.submeshIndex = static_cast<uint32>(buffers.submeshes.size());
    item.packet = MakeDepthPacket(
        scene, 0, item.submeshIndex, buffers, item.material,
        RenderMaterialMode::Opaque, RenderDrawFlags::None);
    // Keep the packet relevant so the execution-stage geometry validation is
    // the component that fails closed.
    item.packet.arguments.indexCount = 3;
    std::vector<RenderDrawItem> opaqueItems = {item};
    std::vector<RenderDrawItem> maskedItems;
    SceneMeshPassPreparation preparation = PrepareDepthPackets(
        opaqueItems, maskedItems);
    const RenderFramePlanCompileResult compiled =
        CompileForcedDirectPlan(preparation, 603);
    ASSERT_TRUE(compiled.succeeded);
    RenderFrameExecutionReport report = MakeExecutionReport(compiled.plan);

    RHITextureRef depthTexture = device.CreateTexture(
        RHITextureDesc::DepthStencil(64, 64, RHIFormat::D32_FLOAT));
    RHITextureViewRef depthView = device.CreateTextureView(depthTexture.Get());
    ASSERT_TRUE(depthView);
    view.viewCache = &viewCache;
    view.renderFrameExecutionPlan = &compiled.plan;
    view.meshPassPreparation = &preparation;
    view.renderFrameExecutionReport = &report;

    DepthPrepass pass;
    pass.SetEnabled(true);
    ConfigureResources(pass, gpuResources, pipelineCache);
    pass.SetMaterialSystem(&materialSystem);
    RecordingCommandContext ctx;
    ExecuteTypedDepthRecording(
        pass, view, opaqueItems, maskedItems, compiled.plan, preparation,
        report, depthView.Get(), nullptr, ctx);

    EXPECT_EQ(0u, ctx.beginRenderPassCount);
    EXPECT_EQ(0u, ctx.drawIndexedCount);
    EXPECT_EQ(RenderPolicyReason::UnexpectedRecordingFailure,
              pass.GetDrawStats().failureReason);
    EXPECT_EQ(RenderExecutionStatus::Failed,
              report.passes.front().directLane.status);
}

TEST_F(RenderPassValidationFixture,
       DepthPrepassRejectsObjectConstantUploadFailureBeforeRecording)
{
    RVX_REQUIRE_RENDER_RUNTIME_PIPELINE();

    scene.GetMutableObject(0).entityId = 75;
    const MeshGPUBuffers buffers =
        gpuResources.GetMeshBuffers(meshResource->GetId());
    ASSERT_TRUE(buffers.IsValid());
    RenderDrawItem item = MakeDrawItem(MaterialRenderMode::Opaque);
    item.packet = MakeDepthPacket(
        scene, 0, 0, buffers, item.material,
        RenderMaterialMode::Opaque, RenderDrawFlags::None);
    std::vector<RenderDrawItem> opaqueItems = {item};
    std::vector<RenderDrawItem> maskedItems;
    SceneMeshPassPreparation preparation = PrepareDepthPackets(
        opaqueItems, maskedItems);
    const RenderFramePlanCompileResult compiled =
        CompileForcedDirectPlan(preparation, 606);
    ASSERT_TRUE(compiled.succeeded);
    RenderFrameExecutionReport report = MakeExecutionReport(compiled.plan);

    RHITextureRef depthTexture = device.CreateTexture(
        RHITextureDesc::DepthStencil(64, 64, RHIFormat::D32_FLOAT));
    RHITextureViewRef depthView = device.CreateTextureView(depthTexture.Get());
    ASSERT_TRUE(depthView);
    view.viewCache = &viewCache;
    view.renderFrameExecutionPlan = &compiled.plan;
    view.meshPassPreparation = &preparation;
    view.renderFrameExecutionReport = &report;

    auto* objectConstantBuffer = static_cast<FakeBuffer*>(
        pipelineCache.m_objectConstantBuffer.Get());
    ASSERT_NE(objectConstantBuffer, nullptr);
    objectConstantBuffer->SetMapSucceeds(false);

    DepthPrepass pass;
    pass.SetEnabled(true);
    ConfigureResources(pass, gpuResources, pipelineCache);
    pass.SetMaterialSystem(&materialSystem);
    RecordingCommandContext ctx;
    ExecuteTypedDepthRecording(
        pass, view, opaqueItems, maskedItems, compiled.plan, preparation,
        report, depthView.Get(), nullptr, ctx);

    EXPECT_EQ(0u, ctx.beginRenderPassCount);
    EXPECT_EQ(0u, ctx.drawIndexedCount);
    EXPECT_EQ(RenderPolicyReason::UnexpectedRecordingFailure,
              pass.GetDrawStats().failureReason);
    EXPECT_EQ(RenderExecutionStatus::Failed,
              report.passes.front().directLane.status);
}

TEST_F(RenderPassValidationFixture,
       DepthPrepassDoesNotReplayDirectAfterPlannedGPULateFailure)
{
    RVX_REQUIRE_RENDER_RUNTIME_PIPELINE();

    scene.GetMutableObject(0).entityId = 76;
    scene.GetMutableObject(0).bounds = meshResource->GetBounds();
    const MeshGPUBuffers buffers =
        gpuResources.GetMeshBuffers(meshResource->GetId());
    ASSERT_TRUE(buffers.IsValid());
    ASSERT_FALSE(buffers.submeshes.empty());

    RenderDrawItem item = MakeDrawItem(MaterialRenderMode::Opaque);
    item.packet = MakeDepthPacket(
        scene, 0, 0, buffers, item.material,
        RenderMaterialMode::Opaque, RenderDrawFlags::None);
    std::vector<RenderDrawItem> opaqueItems = {item};
    std::vector<RenderDrawItem> maskedItems;
    SceneMeshPassPreparation preparation = PrepareDepthPackets(
        opaqueItems, maskedItems);
    const RenderFramePlanCompileResult compiled =
        CompileForcedGPUPlan(preparation);
    ASSERT_TRUE(compiled.succeeded);
    const auto depthPlan = std::find_if(
        compiled.plan.passes.begin(),
        compiled.plan.passes.end(),
        [](const RenderPassExecutionPlan& pass)
        {
            return pass.pass == RenderPassKind::Depth;
        });
    ASSERT_NE(depthPlan, compiled.plan.passes.end());
    ASSERT_GT(depthPlan->partition.gpuDrivenPacketCount, 0u);
    RenderFrameExecutionReport report = MakeExecutionReport(compiled.plan);

    GPUCulling culling;
    GPUCullingConfig cullingConfig;
    cullingConfig.maxInstances = 4;
    cullingConfig.enableOcclusionCulling = false;
    cullingConfig.enableDistanceCulling = false;
    culling.Initialize(&device, cullingConfig);
    ASSERT_NE(culling.GetInstanceBuffer(), nullptr);
    ASSERT_NE(culling.GetInstanceIndexBuffer(), nullptr);
    culling.BeginFrame();
    GPUIndexedDrawDesc drawDesc;
    drawDesc.indexCount = buffers.submeshes[0].indexCount;
    drawDesc.firstIndex = buffers.submeshes[0].indexOffset;
    drawDesc.vertexOffset = buffers.submeshes[0].baseVertex;
    ASSERT_EQ(0u, culling.BeginDrawGroup(meshResource->GetId()));
    EXPECT_NE(RVX_INVALID_INDEX,
              culling.AddDrawItemInstance(scene, item, drawDesc, 0));
    culling.EndDrawGroup();
    culling.EndFrame();
    culling.CullCpuFallback(view.viewMatrix, view.projectionMatrix);
    ASSERT_TRUE(culling.WasCpuFallbackUsedLastCull());

    RHITextureRef depthTexture = device.CreateTexture(
        RHITextureDesc::DepthStencil(64, 64, RHIFormat::D32_FLOAT));
    RHITextureViewRef depthView = device.CreateTextureView(depthTexture.Get());
    ASSERT_TRUE(depthView);
    view.viewCache = &viewCache;
    view.renderFrameExecutionPlan = &compiled.plan;
    view.meshPassPreparation = &preparation;
    view.renderFrameExecutionReport = &report;

    auto* objectConstantBuffer = static_cast<FakeBuffer*>(
        pipelineCache.m_objectConstantBuffer.Get());
    ASSERT_NE(objectConstantBuffer, nullptr);
    objectConstantBuffer->SetMapSucceeds(false);

    DepthPrepass pass;
    pass.SetEnabled(true);
    ConfigureResources(pass, gpuResources, pipelineCache);
    RecordingCommandContext ctx;
    ExecuteTypedDepthRecording(
        pass, view, opaqueItems, maskedItems, compiled.plan, preparation,
        report, depthView.Get(), &culling, ctx);

    EXPECT_EQ(1u, ctx.beginRenderPassCount);
    EXPECT_EQ(1u, ctx.endRenderPassCount);
    EXPECT_EQ(0u, ctx.drawIndexedCount);
    EXPECT_EQ(0u, ctx.drawIndexedIndirectCount);
    EXPECT_EQ(RenderPolicyReason::UnexpectedRecordingFailure,
              pass.GetDrawStats().failureReason);
    EXPECT_EQ(RenderExecutionStatus::Failed, report.status);
    ASSERT_FALSE(report.passes.empty());
    const RenderPassExecutionReport& depthReport = report.passes.front();
    EXPECT_EQ(RenderExecutionStatus::Failed, depthReport.status);
    EXPECT_EQ(RenderExecutionStatus::Failed,
              depthReport.gpuDrivenLane.status);
    EXPECT_EQ(RenderExecutionStatus::NotAttempted,
              depthReport.directLane.status);
}

TEST_F(RenderPassValidationFixture,
       DepthPrepassGPUSceneInputMismatchSignalsFrameAbort)
{
    RVX_REQUIRE_RENDER_RUNTIME_PIPELINE();

    scene.GetMutableObject(0).entityId = 7601;
    scene.GetMutableObject(0).bounds = meshResource->GetBounds();
    const MeshGPUBuffers buffers =
        gpuResources.GetMeshBuffers(meshResource->GetId());
    ASSERT_TRUE(buffers.IsValid());
    ASSERT_FALSE(buffers.submeshes.empty());

    RenderDrawItem item = MakeDrawItem(MaterialRenderMode::Opaque);
    item.packet = MakeDepthPacket(
        scene, 0, 0, buffers, item.material,
        RenderMaterialMode::Opaque, RenderDrawFlags::None);
    std::vector<RenderDrawItem> opaqueItems = {item};
    std::vector<RenderDrawItem> maskedItems;
    SceneMeshPassPreparation preparation = PrepareDepthPackets(
        opaqueItems, maskedItems);
    const RenderFramePlanCompileResult compiled =
        CompileForcedGPUPlan(preparation, 7601);
    ASSERT_TRUE(compiled.succeeded);
    const auto plannedDepth = std::find_if(
        compiled.plan.passes.begin(), compiled.plan.passes.end(),
        [](const RenderPassExecutionPlan& passPlan)
        {
            return passPlan.pass == RenderPassKind::Depth;
        });
    ASSERT_NE(plannedDepth, compiled.plan.passes.end());
    ASSERT_EQ(1u, plannedDepth->partition.gpuDrivenPacketCount);
    RenderFrameExecutionReport report = MakeExecutionReport(compiled.plan);

    GPUCulling culling;
    GPUCullingConfig cullingConfig;
    cullingConfig.maxInstances = 4;
    cullingConfig.enableOcclusionCulling = false;
    cullingConfig.enableDistanceCulling = false;
    culling.Initialize(&device, cullingConfig);
    culling.BeginFrame();
    GPUIndexedDrawDesc drawDesc;
    drawDesc.indexCount = buffers.submeshes[0].indexCount;
    drawDesc.firstIndex = buffers.submeshes[0].indexOffset;
    drawDesc.vertexOffset = buffers.submeshes[0].baseVertex;
    ASSERT_EQ(0u, culling.BeginDrawGroup(
        meshResource->GetId(), item.material.slot,
        MaterialPipelineVariant::Opaque, item.mesh, item.material));
    ASSERT_NE(RVX_INVALID_INDEX,
              culling.AddDrawItemInstance(scene, item, drawDesc, 0));
    culling.EndDrawGroup();
    culling.EndFrame();
    culling.CullCpuFallback(view.viewMatrix, view.projectionMatrix);

    RenderGraph graph;
    graph.SetDevice(&device);
    RHITextureRef depthTexture = device.CreateTexture(
        RHITextureDesc::DepthStencil(64, 64, RHIFormat::D32_FLOAT));
    RHITextureViewRef depthView = device.CreateTextureView(depthTexture.Get());
    ASSERT_TRUE(depthView);
    ViewData frameView = view;
    frameView.viewCache = &viewCache;
    frameView.depthTarget = graph.ImportTexture(
        depthTexture.Get(), RHIResourceState::DepthWrite);
    RenderPassRecordContext context = MakeMainSceneRecordContext(
        graph, frameView, scene, opaqueItems, maskedItems, compiled.plan,
        preparation, report, compiled.plan.frameSequence);
    context.depthGPUDriven = MakeSyntheticGPUSceneRasterInputs(
        graph, device, pipelineCache,
        MakeMainSceneGPUDrivenInputs(graph, context.identity, culling));
    ASSERT_TRUE(context.depthGPUDriven.IsCompatibleWith(context.identity));
    const std::shared_ptr<std::atomic_bool> recordingFailure =
        context.depthGPUDriven.gpuSceneRecordingFailure;
    ++context.depthGPUDriven.gpuSceneLeaseVersion;
    ASSERT_FALSE(context.depthGPUDriven.IsCompatibleWith(context.identity));

    DepthPrepass pass;
    pass.SetEnabled(true);
    ConfigureResources(pass, gpuResources, pipelineCache);
    // A stale exact lease must reject the typed input at graph registration
    // and propagate the shared frame-abort signal without recording work.
    pass.AddToGraph(graph, context);
    graph.Compile();
    ASSERT_TRUE(graph.GetCompileStats().compileValid);
    RecordingCommandContext commands;
    graph.Execute(commands);
    pass.PublishRecordResults(context.results, context.identity);

    ASSERT_TRUE(recordingFailure);
    EXPECT_TRUE(recordingFailure->load());
    EXPECT_EQ(0u, commands.beginRenderPassCount);
    EXPECT_EQ(0u, commands.drawIndexedIndirectCount);
}

TEST_F(RenderPassValidationFixture,
       DepthPrepassUsesPublishedPacketsAfterDrawItemDrift)
{
    RVX_REQUIRE_RENDER_RUNTIME_PIPELINE();

    scene.GetMutableObject(0).entityId = 74;
    const MeshGPUBuffers buffers =
        gpuResources.GetMeshBuffers(meshResource->GetId());
    ASSERT_TRUE(buffers.IsValid());
    RenderDrawItem item = MakeDrawItem(MaterialRenderMode::Opaque);
    item.packet = MakeDepthPacket(
        scene, 0, 0, buffers, item.material,
        RenderMaterialMode::Opaque, RenderDrawFlags::None);
    std::vector<RenderDrawItem> opaqueItems = {item};
    std::vector<RenderDrawItem> maskedItems;
    SceneMeshPassPreparation preparation = PrepareDepthPackets(
        opaqueItems, maskedItems);
    const RenderFramePlanCompileResult compiled =
        CompileForcedDirectPlan(preparation, 604);
    ASSERT_TRUE(compiled.succeeded);
    RenderFrameExecutionReport report = MakeExecutionReport(compiled.plan);
    opaqueItems.front().renderMode = RenderMaterialMode::Masked;

    RHITextureRef depthTexture = device.CreateTexture(
        RHITextureDesc::DepthStencil(64, 64, RHIFormat::D32_FLOAT));
    RHITextureViewRef depthView = device.CreateTextureView(depthTexture.Get());
    ASSERT_TRUE(depthView);
    view.viewCache = &viewCache;
    view.renderFrameExecutionPlan = &compiled.plan;
    view.meshPassPreparation = &preparation;
    view.renderFrameExecutionReport = &report;

    DepthPrepass pass;
    pass.SetEnabled(true);
    ConfigureResources(pass, gpuResources, pipelineCache);
    pass.SetMaterialSystem(&materialSystem);
    RecordingCommandContext ctx;
    ExecuteTypedDepthRecording(
        pass, view, opaqueItems, maskedItems, compiled.plan, preparation,
        report, depthView.Get(), nullptr, ctx);

    EXPECT_EQ(1u, ctx.beginRenderPassCount);
    EXPECT_EQ(1u, ctx.endRenderPassCount);
    EXPECT_EQ(1u, ctx.drawIndexedCount);
    EXPECT_TRUE(pass.GetDrawStats().planValidated);
    EXPECT_TRUE(pass.GetDrawStats().directPacketPathUsed);
    EXPECT_EQ(RenderPolicyReason::None,
              pass.GetDrawStats().failureReason);
}

TEST_F(RenderPassValidationFixture,
       DepthPrepassClearsForEmptyPlannedDirectRange)
{
    RVX_REQUIRE_RENDER_RUNTIME_PIPELINE();

    std::vector<RenderDrawItem> opaqueItems;
    std::vector<RenderDrawItem> maskedItems;
    SceneMeshPassPreparation preparation = PrepareDepthPackets(
        opaqueItems, maskedItems);
    const RenderFramePlanCompileResult compiled =
        CompileForcedDirectPlan(preparation, 605);
    ASSERT_TRUE(compiled.succeeded);
    RenderFrameExecutionReport report = MakeExecutionReport(compiled.plan);

    RHITextureRef depthTexture = device.CreateTexture(
        RHITextureDesc::DepthStencil(64, 64, RHIFormat::D32_FLOAT));
    RHITextureViewRef depthView = device.CreateTextureView(depthTexture.Get());
    ASSERT_TRUE(depthView);
    view.viewCache = &viewCache;
    view.renderFrameExecutionPlan = &compiled.plan;
    view.meshPassPreparation = &preparation;
    view.renderFrameExecutionReport = &report;

    DepthPrepass pass;
    pass.SetEnabled(true);
    ConfigureResources(pass, gpuResources, pipelineCache);
    pass.SetMaterialSystem(&materialSystem);
    RecordingCommandContext ctx;
    ExecuteTypedDepthRecording(
        pass, view, opaqueItems, maskedItems, compiled.plan, preparation,
        report, depthView.Get(), nullptr, ctx);

    EXPECT_EQ(1u, ctx.beginRenderPassCount);
    EXPECT_EQ(1u, ctx.endRenderPassCount);
    EXPECT_EQ(0u, ctx.drawIndexedCount);
    EXPECT_TRUE(pass.GetDrawStats().directPacketPathUsed);
    EXPECT_TRUE(pass.GetDrawStats().planValidated);
    EXPECT_EQ(RenderExecutionStatus::Completed,
              report.passes.front().directLane.status);
}

TEST_F(RenderPassValidationFixture,
       DepthPrepassClearsNonEmptyAllSkipPlanWithoutExecutingDirectLane)
{
    RVX_REQUIRE_RENDER_RUNTIME_PIPELINE();

    scene.GetMutableObject(0).entityId = 91;
    const MeshGPUBuffers buffers =
        gpuResources.GetMeshBuffers(meshResource->GetId());
    ASSERT_TRUE(buffers.IsValid());
    RenderDrawItem item = MakeDrawItem(MaterialRenderMode::Opaque);
    item.packet = MakeDepthPacket(
        scene, 0, 0, buffers, item.material,
        RenderMaterialMode::Opaque, RenderDrawFlags::None);
    std::vector<RenderDrawItem> opaqueItems = {item};
    std::vector<RenderDrawItem> maskedItems;
    SceneMeshPassPreparation preparation = PrepareDepthPackets(
        opaqueItems, maskedItems);
    const RenderFramePlanCompileResult compiled = CompilePlan(
        preparation,
        RenderGPUDrivenMode::ForceDisabled,
        610,
        RenderPolicyReadiness::Pending);
    ASSERT_TRUE(compiled.succeeded);
    const RenderPassExecutionPlan& depthPlan = compiled.plan.passes[0];
    EXPECT_EQ(0u, depthPlan.partition.gpuDrivenPacketCount);
    EXPECT_EQ(0u, depthPlan.partition.directPacketCount);
    EXPECT_EQ(1u, depthPlan.partition.skippedPacketCount);
    EXPECT_EQ(RenderPolicyReason::ResourcesPending, depthPlan.reason);
    RenderFrameExecutionReport report = MakeExecutionReport(compiled.plan);

    RHITextureRef depthTexture = device.CreateTexture(
        RHITextureDesc::DepthStencil(64, 64, RHIFormat::D32_FLOAT));
    RHITextureViewRef depthView = device.CreateTextureView(depthTexture.Get());
    ASSERT_TRUE(depthView);
    view.viewCache = &viewCache;
    view.renderFrameExecutionPlan = &compiled.plan;
    view.meshPassPreparation = &preparation;
    view.renderFrameExecutionReport = &report;

    DepthPrepass pass;
    pass.SetEnabled(true);
    ConfigureResources(pass, gpuResources, pipelineCache);
    pass.SetMaterialSystem(&materialSystem);
    RecordingCommandContext ctx;
    ExecuteTypedDepthRecording(
        pass, view, opaqueItems, maskedItems, compiled.plan, preparation,
        report, depthView.Get(), nullptr, ctx);

    EXPECT_EQ(1u, ctx.beginRenderPassCount);
    EXPECT_EQ(1u, ctx.endRenderPassCount);
    EXPECT_EQ(0u, ctx.drawIndexedIndirectCount);
    EXPECT_EQ(0u, ctx.drawIndexedCount);
    EXPECT_TRUE(pass.GetDrawStats().planValidated);
    EXPECT_FALSE(pass.GetDrawStats().directPacketPathUsed);
    EXPECT_EQ(RenderPolicyReason::None,
              pass.GetDrawStats().failureReason);

    const RenderPassExecutionReport* depthReport =
        FindPassExecutionReport(report, RenderPassKind::Depth);
    ASSERT_NE(depthReport, nullptr);
    EXPECT_EQ(RenderExecutionStatus::Completed, depthReport->status);
    EXPECT_EQ(RenderExecutionStatus::NotAttempted,
              depthReport->gpuDrivenLane.status);
    EXPECT_EQ(RenderExecutionStatus::NotAttempted,
              depthReport->directLane.status);
    EXPECT_EQ(1u, depthReport->skippedPacketCount);
    EXPECT_EQ(RenderPolicyReason::ResourcesPending,
              depthReport->reason);
}

TEST_F(RenderPassValidationFixture,
       OpaquePassRecordsPlannedOpaqueMaskedAndDefaultMaterialPackets)
{
    RVX_REQUIRE_RENDER_RUNTIME_PIPELINE();

    scene.GetMutableObject(0).entityId = 81;
    const MeshGPUBuffers buffers =
        gpuResources.GetMeshBuffers(meshResource->GetId());
    ASSERT_TRUE(buffers.IsValid());
    ASSERT_NE(buffers.normalBuffer, nullptr);
    ASSERT_NE(buffers.uvBuffer, nullptr);
    ASSERT_FALSE(buffers.HasNormalMapTangentBasis());

    RenderDrawItem opaqueItem = MakeDrawItem(MaterialRenderMode::Opaque);
    opaqueItem.packet = MakeOpaquePacket(
        scene, 0, 0, buffers, opaqueItem.material,
        RenderMaterialMode::Opaque, RenderDrawFlags::None);
    RenderDrawItem maskedItem = MakeDrawItem(MaterialRenderMode::Masked);
    maskedItem.material = {};
    maskedItem.packet = MakeOpaquePacket(
        scene, 0, 0, buffers, {}, RenderMaterialMode::Masked,
        RenderDrawFlags::Masked | RenderDrawFlags::MissingMaterial);
    std::vector<RenderDrawItem> opaqueItems = {opaqueItem};
    std::vector<RenderDrawItem> maskedItems = {maskedItem};

    SceneMeshPassPreparation preparation = PrepareOpaquePackets(
        opaqueItems, maskedItems);
    const RenderFramePlanCompileResult compiled =
        CompileForcedDirectPlan(preparation, 611);
    ASSERT_TRUE(compiled.succeeded);
    RenderFrameExecutionReport report = MakeExecutionReport(compiled.plan);

    view.viewCache = &viewCache;
    view.renderFrameExecutionPlan = &compiled.plan;
    view.meshPassPreparation = &preparation;
    view.renderFrameExecutionReport = &report;
    view.viewportWidth = 64;
    view.viewportHeight = 64;

    RenderGraph graph;
    graph.SetDevice(&device);
    RHITextureDesc plannedColorDesc = RHITextureDesc::RenderTarget(
        64, 64, RHIFormat::RGBA16_FLOAT);
    plannedColorDesc.debugName = "PlannedOpaqueRGBA16Color";
    view.colorTarget = graph.CreateTexture(plannedColorDesc);
    graph.SetExportState(view.colorTarget, RHIResourceState::RenderTarget);
    view.renderGraph = &graph;

    OpaquePass pass;
    pass.OnAdd(&device);
    ConfigureResources(pass, gpuResources, pipelineCache, materialSystem);
    RenderPassRecordContext context = MakeMainSceneRecordContext(
        graph, view, scene, opaqueItems, maskedItems, compiled.plan,
        preparation, report, compiled.plan.frameSequence);
    pass.AddToGraph(graph, context);
    graph.Compile();
    RecordingCommandContext ctx;
    graph.Execute(ctx);
    pass.PublishRecordResults(context.results, context.identity);
    report = context.results->executionReport;

    const OpaquePassDrawStats& stats = pass.GetDrawStats();
    EXPECT_TRUE(stats.planRequested);
    EXPECT_TRUE(stats.planValidated);
    EXPECT_TRUE(stats.directPacketPathUsed);
    EXPECT_EQ(2u, stats.plannedPacketCount);
    EXPECT_EQ(2u, stats.executedPacketCount);
    EXPECT_EQ(2u, stats.directDrawCount);

    EXPECT_EQ(1u, ctx.beginRenderPassCount);
    EXPECT_EQ(1u, ctx.endRenderPassCount);
    EXPECT_EQ(2u, ctx.drawIndexedCount);
    EXPECT_EQ(0u, ctx.drawIndexedIndirectCount);
    ASSERT_EQ(2u, ctx.pipelineSequence.size());
    EXPECT_EQ(pipelineCache.GetPipelineForVariant(
                  MaterialPipelineVariant::Opaque,
                  RHIFormat::RGBA16_FLOAT,
                  DefaultLitDirectVertexInputMode::Rigid),
              ctx.pipelineSequence[0]);
    EXPECT_EQ(pipelineCache.GetPipelineForVariant(
                  MaterialPipelineVariant::Masked,
                  RHIFormat::RGBA16_FLOAT,
                  DefaultLitDirectVertexInputMode::Rigid),
              ctx.pipelineSequence[1]);
    ASSERT_EQ(2u, ctx.drawIndexedVertexBuffers.size());
    for (const auto& vertexBuffers : ctx.drawIndexedVertexBuffers)
    {
        EXPECT_NE(vertexBuffers[0], nullptr);
        EXPECT_NE(vertexBuffers[1], nullptr);
        EXPECT_NE(vertexBuffers[2], nullptr);
        EXPECT_NE(vertexBuffers[3], nullptr);
        EXPECT_EQ(vertexBuffers[4], nullptr);
        EXPECT_EQ(vertexBuffers[5], nullptr);
    }
    EXPECT_TRUE(std::any_of(ctx.descriptorSetSequence.begin(),
                            ctx.descriptorSetSequence.end(),
                            [](uint32 set) { return set == 2; }));

    const RenderPassExecutionReport* opaqueReport =
        FindPassExecutionReport(report, RenderPassKind::Opaque);
    ASSERT_NE(opaqueReport, nullptr);
    EXPECT_EQ(RenderExecutionStatus::Completed, opaqueReport->status);
    EXPECT_EQ(RenderExecutionStatus::Completed,
              opaqueReport->directLane.status);
    EXPECT_EQ(RenderExecutionStatus::NotAttempted,
              opaqueReport->gpuDrivenLane.status);
    EXPECT_EQ(2u, opaqueReport->directLane.executedPacketCount);
}

TEST_F(RenderPassValidationFixture,
       OpaquePassRecordsPlannedSkinnedMultiSubmeshMultiMaterialPackets)
{
    RVX_REQUIRE_RENDER_RUNTIME_PIPELINE();

    Resource::MaterialResource firstMaterial;
    firstMaterial.SetId(713);
    firstMaterial.SetName("PlannedOpaqueFirstMaterial");
    firstMaterial.SetMaterialData(std::make_shared<Material>());
    Resource::MaterialResource secondMaterial;
    secondMaterial.SetId(714);
    secondMaterial.SetName("PlannedOpaqueSecondMaterial");
    secondMaterial.SetMaterialData(std::make_shared<Material>());
    ASSERT_TRUE(gpuResources.UploadImmediate(&firstMaterial));
    ASSERT_TRUE(gpuResources.UploadImmediate(&secondMaterial));

    auto skinnedMesh = CreateTwoSubmeshMeshResource(1403);
    skinnedMesh->GetMesh()->SetBoneData(
        std::vector<IVec4>(6, IVec4(0, 0, 0, 0)),
        std::vector<Vec4>(6, Vec4(1.0f, 0.0f, 0.0f, 0.0f)));
    ASSERT_TRUE(gpuResources.UploadImmediate(skinnedMesh.get()));
    const MeshGPUBuffers buffers =
        gpuResources.GetMeshBuffers(skinnedMesh->GetId());
    ASSERT_TRUE(buffers.IsValid());
    ASSERT_TRUE(buffers.HasSkinningVertexData());
    ASSERT_EQ(2u, buffers.submeshes.size());

    RenderObject object = MakeRenderObject(*skinnedMesh, gpuResources);
    object.entityId = 82;
    object.receivesShadow = false;
    object.previousWorldMatrix = Mat4Identity();
    object.previousWorldMatrixValid = 1;
    object.skinningMatrices.push_back(Mat4Identity());
    scene.Clear();
    scene.AddObject(object);

    RenderDrawItem firstItem;
    firstItem.objectIndex = 0;
    firstItem.submeshIndex = 0;
    firstItem.mesh = object.mesh;
    firstItem.material = gpuResources.GetHandle(firstMaterial.GetId());
    firstItem.renderMode = RenderMaterialMode::Opaque;
    firstItem.packet = MakeOpaquePacket(
        scene, 0, 0, buffers, firstItem.material,
        RenderMaterialMode::Opaque, RenderDrawFlags::Skinned);
    RenderDrawItem secondItem = firstItem;
    secondItem.submeshIndex = 1;
    secondItem.material = gpuResources.GetHandle(secondMaterial.GetId());
    secondItem.packet = MakeOpaquePacket(
        scene, 0, 1, buffers, secondItem.material,
        RenderMaterialMode::Opaque, RenderDrawFlags::Skinned);
    std::vector<RenderDrawItem> opaqueItems = {firstItem, secondItem};
    std::vector<RenderDrawItem> maskedItems;

    SceneMeshPassPreparation preparation = PrepareOpaquePackets(
        opaqueItems, maskedItems);
    const RenderFramePlanCompileResult compiled =
        CompileForcedDirectPlan(preparation, 612);
    ASSERT_TRUE(compiled.succeeded);
    RenderFrameExecutionReport report = MakeExecutionReport(compiled.plan);
    view.viewCache = &viewCache;
    view.previousViewProjectionMatrix = Mat4Identity();
    view.previousViewProjectionValid = 1;
    view.renderFrameExecutionPlan = &compiled.plan;
    view.meshPassPreparation = &preparation;
    view.renderFrameExecutionReport = &report;

    OpaquePass pass;
    pass.OnAdd(&device);
    ConfigureResources(pass, gpuResources, pipelineCache, materialSystem);
    RecordingCommandContext ctx;
    ExecuteTypedOpaqueRecording(
        pass, view, opaqueItems, maskedItems, compiled.plan, preparation,
        report, colorView.Get(), nullptr, nullptr, ctx);

    EXPECT_EQ(1u, ctx.beginRenderPassCount);
    EXPECT_EQ(2u, ctx.drawIndexedCount);
    EXPECT_EQ((std::vector<uint32>{0u, 3u}), ctx.drawIndexedFirstIndices);
    EXPECT_EQ(0u, ctx.drawIndexedIndirectCount);
    EXPECT_EQ(2u, pass.GetDrawStats().executedPacketCount);
    ASSERT_EQ(2u, ctx.drawIndexedVertexBuffers.size());
    for (const auto& vertexBuffers : ctx.drawIndexedVertexBuffers)
    {
        for (uint32 slot = 0; slot <= 5; ++slot)
        {
            EXPECT_NE(vertexBuffers[slot], nullptr);
        }
    }

    const FakeBuffer* objectConstantsBuffer =
        FindCreatedBuffer(device, "ObjectConstantBuffer");
    ASSERT_NE(objectConstantsBuffer, nullptr);
    ASSERT_GE(objectConstantsBuffer->GetStorage().size(),
              sizeof(ObjectConstants));
    ObjectConstants uploaded{};
    std::memcpy(&uploaded,
                objectConstantsBuffer->GetStorage().data(),
                sizeof(uploaded));
    EXPECT_FLOAT_EQ(1.0f, uploaded.objectVelocityParams.x);
    EXPECT_FLOAT_EQ(0.0f, uploaded.objectVelocityParams.y);
    EXPECT_FLOAT_EQ(1.0f, uploaded.skinningParams.x);
    EXPECT_FLOAT_EQ(1.0f, uploaded.skinningParams.y);
}

TEST_F(RenderPassValidationFixture,
       OpaquePassRecordsMixedPublishedGPUAndDirectLanesInSingleRenderPass)
{
    RVX_REQUIRE_RENDER_RUNTIME_PIPELINE();

    scene.GetMutableObject(0).entityId = 87;
    scene.GetMutableObject(0).bounds = meshResource->GetBounds();
    const MeshGPUBuffers staticBuffers =
        gpuResources.GetMeshBuffers(meshResource->GetId());
    ASSERT_TRUE(staticBuffers.IsValid());
    ASSERT_FALSE(staticBuffers.submeshes.empty());

    auto skinnedMesh = CreateTwoSubmeshMeshResource(1421);
    skinnedMesh->GetMesh()->SetBoneData(
        std::vector<IVec4>(6, IVec4(0, 0, 0, 0)),
        std::vector<Vec4>(6, Vec4(1.0f, 0.0f, 0.0f, 0.0f)));
    ASSERT_TRUE(gpuResources.UploadImmediate(skinnedMesh.get()));
    const MeshGPUBuffers skinnedBuffers =
        gpuResources.GetMeshBuffers(skinnedMesh->GetId());
    ASSERT_TRUE(skinnedBuffers.IsValid());
    ASSERT_TRUE(skinnedBuffers.HasSkinningVertexData());
    ASSERT_GE(skinnedBuffers.submeshes.size(), 2u);

    RenderObject skinnedObject =
        MakeRenderObject(*skinnedMesh, gpuResources);
    skinnedObject.entityId = 88;
    skinnedObject.bounds = skinnedMesh->GetBounds();
    skinnedObject.skinningMatrices.push_back(Mat4Identity());
    scene.AddObject(skinnedObject);

    RenderObject specialObject =
        MakeRenderObject(*meshResource, gpuResources);
    specialObject.entityId = 90;
    specialObject.bounds = meshResource->GetBounds();
    scene.AddObject(specialObject);

    RenderDrawItem gpuCandidate = MakeDrawItem(MaterialRenderMode::Opaque);
    gpuCandidate.packet = MakeOpaquePacket(
        scene, 0, 0, staticBuffers, gpuCandidate.material,
        RenderMaterialMode::Opaque, RenderDrawFlags::None);
    RenderDrawItem directItem = MakeDrawItem(MaterialRenderMode::Opaque);
    directItem.objectIndex = 1;
    directItem.submeshIndex = 1;
    directItem.mesh = skinnedObject.mesh;
    directItem.packet = MakeOpaquePacket(
        scene, 1, 1, skinnedBuffers, directItem.material,
        RenderMaterialMode::Opaque, RenderDrawFlags::Skinned);
    RenderDrawItem specialMissingItem =
        MakeDrawItem(MaterialRenderMode::Opaque);
    specialMissingItem.objectIndex = 2;
    specialMissingItem.mesh = specialObject.mesh;
    specialMissingItem.material = {};
    specialMissingItem.packet = MakeOpaquePacket(
        scene, 2, 0, staticBuffers, {}, RenderMaterialMode::Opaque,
        RenderDrawFlags::MissingMaterial);
    RenderDrawItem transparentItem =
        MakeDrawItem(MaterialRenderMode::Transparent);
    transparentItem.packet = MakeOpaquePacket(
        scene, 0, 0, staticBuffers, transparentItem.material,
        RenderMaterialMode::Opaque, RenderDrawFlags::None);
    transparentItem.packet.pipelineKey.materialVariant =
        MaterialPipelineVariant::Transparent;
    transparentItem.packet.materialKey.materialMode =
        MaterialRenderMode::Transparent;
    std::vector<RenderDrawItem> opaqueItems = {
        gpuCandidate, directItem, specialMissingItem};
    std::vector<RenderDrawItem> maskedItems;
    std::vector<RenderDrawItem> transparentItems = {transparentItem};

    SceneMeshPassPreparation preparation;
    OpaqueMeshPassProcessor opaqueProcessor;
    for (uint32 index = 0;
         index < static_cast<uint32>(opaqueItems.size());
         ++index)
    {
        MeshPassProcessorInput input;
        input.packet = opaqueItems[index].packet;
        input.sourceOrdinal = index;
        input.availability.specialMaterial = index == 2;
        preparation.opaque.Record(opaqueProcessor.Process(input));
    }
    TransparentMeshPassProcessor transparentProcessor;
    MeshPassProcessorInput transparentInput;
    transparentInput.packet = transparentItem.packet;
    transparentInput.sourceOrdinal = 0;
    preparation.transparent.Record(
        transparentProcessor.Process(transparentInput));
    preparation.depth.FinalizeGroups();
    preparation.opaque.FinalizeGroups();
    preparation.shadow.FinalizeGroups();
    preparation.transparent.FinalizeGroups();
    ASSERT_EQ(MeshPassEligibilityReason::SpecialMaterial,
              preparation.opaque.packets[2].reason);
    ASSERT_EQ(MeshPassEligibilityReason::Transparent,
              preparation.transparent.packets[0].reason);
    const RenderFramePlanCompileResult compiled =
        CompileForcedGPUPlan(preparation, 618);
    ASSERT_TRUE(compiled.succeeded);
    const auto opaquePlan = std::find_if(
        compiled.plan.passes.begin(),
        compiled.plan.passes.end(),
        [](const RenderPassExecutionPlan& passPlan)
        {
            return passPlan.pass == RenderPassKind::Opaque;
        });
    ASSERT_NE(opaquePlan, compiled.plan.passes.end());
    EXPECT_EQ(1u, opaquePlan->partition.gpuDrivenPacketCount);
    EXPECT_EQ(2u, opaquePlan->partition.directPacketCount);
    EXPECT_EQ(0u, opaquePlan->partition.skippedPacketCount);
    EXPECT_EQ(1u, opaquePlan->partition.drawGroupCount);
    EXPECT_EQ(3u, opaquePlan->identityAccounting.expectedPacketCount);
    EXPECT_TRUE(opaquePlan->identityAccounting.IsExactlyOnce());
    const auto transparentPlan = std::find_if(
        compiled.plan.passes.begin(),
        compiled.plan.passes.end(),
        [](const RenderPassExecutionPlan& passPlan)
        {
            return passPlan.pass == RenderPassKind::Transparent;
        });
    ASSERT_NE(transparentPlan, compiled.plan.passes.end());
    EXPECT_EQ(0u, transparentPlan->partition.gpuDrivenPacketCount);
    EXPECT_EQ(1u, transparentPlan->partition.directPacketCount);
    EXPECT_TRUE(transparentPlan->identityAccounting.IsExactlyOnce());
    RenderFrameExecutionReport report = MakeExecutionReport(compiled.plan);

    GPUCulling culling;
    GPUCullingConfig cullingConfig;
    cullingConfig.maxInstances = 4;
    cullingConfig.enableOcclusionCulling = false;
    cullingConfig.enableDistanceCulling = false;
    culling.Initialize(&device, cullingConfig);
    culling.BeginFrame();
    GPUIndexedDrawDesc drawDesc;
    drawDesc.indexCount = staticBuffers.submeshes[0].indexCount;
    drawDesc.firstIndex = staticBuffers.submeshes[0].indexOffset;
    drawDesc.vertexOffset = staticBuffers.submeshes[0].baseVertex;
    ASSERT_EQ(0u, culling.BeginDrawGroup(
        meshResource->GetId(),
        gpuCandidate.material.slot,
        MaterialPipelineVariant::Opaque,
        gpuCandidate.mesh,
        gpuCandidate.material));
    EXPECT_NE(RVX_INVALID_INDEX,
              culling.AddDrawItemInstance(scene, gpuCandidate, drawDesc, 0));
    culling.EndDrawGroup();
    culling.EndFrame();
    culling.CullCpuFallback(view.viewMatrix, view.projectionMatrix);
    ASSERT_EQ(1u, culling.GetDrawGroups().size());
    EXPECT_EQ(1u, culling.GetDrawGroups()[0].maxDrawCount);
    EXPECT_EQ(1u, culling.GetInstanceCount());

    view.viewCache = &viewCache;
    view.renderFrameExecutionPlan = &compiled.plan;
    view.meshPassPreparation = &preparation;
    view.renderFrameExecutionReport = &report;

    OpaquePass pass;
    ConfigureResources(pass, gpuResources, pipelineCache, materialSystem);
    RecordingCommandContext ctx;
    ExecuteTypedOpaqueRecording(
        pass, view, opaqueItems, maskedItems, compiled.plan, preparation,
        report, colorView.Get(), nullptr, &culling, ctx);

    EXPECT_EQ(1u, ctx.beginRenderPassCount);
    EXPECT_EQ(1u, ctx.endRenderPassCount);
    ASSERT_EQ(1u, ctx.renderPasses.size());
    ASSERT_EQ(1u, ctx.renderPasses[0].colorAttachmentCount);
    EXPECT_EQ(RHILoadOp::Clear,
              ctx.renderPasses[0].colorAttachments[0].loadOp);
    EXPECT_EQ(1u, ctx.drawIndexedIndirectCount);
    EXPECT_EQ(2u, ctx.drawIndexedCount);

    const OpaquePassDrawStats& stats = pass.GetDrawStats();
    EXPECT_TRUE(stats.planRequested);
    EXPECT_TRUE(stats.planValidated);
    EXPECT_EQ(3u, stats.plannedPacketCount);
    EXPECT_EQ(1u, stats.gpuDrivenIndirectDrawCount);
    EXPECT_EQ(2u, stats.directDrawCount);
    EXPECT_EQ(2u, stats.executedPacketCount);
    EXPECT_EQ(RenderPolicyReason::None, stats.failureReason);

    const RenderPassExecutionReport* opaqueReport =
        FindPassExecutionReport(report, RenderPassKind::Opaque);
    ASSERT_NE(opaqueReport, nullptr);
    EXPECT_EQ(RenderExecutionStatus::Completed, opaqueReport->status);
    EXPECT_EQ(RenderExecutionStatus::Completed,
              opaqueReport->gpuDrivenLane.status);
    EXPECT_EQ(RenderExecutionStatus::Completed,
              opaqueReport->directLane.status);
    EXPECT_EQ(1u, opaqueReport->gpuDrivenLane.executedPacketCount);
    EXPECT_EQ(2u, opaqueReport->directLane.executedPacketCount);

    TransparentPass transparentPass;
    ConfigureResources(transparentPass,
                       gpuResources,
                       pipelineCache,
                       materialSystem);
    RenderGraph transparentGraph;
    transparentGraph.SetDevice(&device);
    RHITextureRef transparentColor = device.CreateTexture(
        RHITextureDesc::RenderTarget(64, 64, RHIFormat::RGBA8_UNORM));
    ASSERT_TRUE(transparentColor);
    ViewData transparentView = view;
    // This independent graph uses a new record epoch. It must not inherit the
    // Opaque test's frame-618 plan/report pointers for a frame-619 recording.
    transparentView.renderFrameExecutionPlan = nullptr;
    transparentView.meshPassPreparation = nullptr;
    transparentView.renderVisibility = nullptr;
    transparentView.renderFrameExecutionReport = nullptr;
    transparentView.colorTarget = transparentGraph.ImportTexture(
        transparentColor.Get(), RHIResourceState::RenderTarget);
    RenderPassRecordContext transparentContext = MakeTransparentRecordContext(
        transparentGraph, transparentView, scene, transparentItems, 619, 619);
    transparentPass.AddToGraph(transparentGraph, transparentContext);
    transparentGraph.Compile();
    ASSERT_TRUE(transparentGraph.GetCompileStats().compileValid);
    RecordingCommandContext transparentCtx;
    transparentGraph.Execute(transparentCtx);
    ASSERT_EQ(1u, transparentCtx.drawIndexedCount);
    ASSERT_EQ(1u, transparentCtx.pipelineSequence.size());
    EXPECT_NE(nullptr, transparentCtx.pipelineSequence[0]);
}

TEST_F(RenderPassValidationFixture,
       OpaquePassExecutesGPUWhilePendingDirectSourceRemainsSkipped)
{
    RVX_REQUIRE_RENDER_RUNTIME_PIPELINE();

    scene.GetMutableObject(0).entityId = 92;
    scene.GetMutableObject(0).bounds = meshResource->GetBounds();
    const MeshGPUBuffers buffers =
        gpuResources.GetMeshBuffers(meshResource->GetId());
    ASSERT_TRUE(buffers.IsValid());
    ASSERT_FALSE(buffers.submeshes.empty());

    RenderDrawItem gpuCandidate = MakeDrawItem(MaterialRenderMode::Opaque);
    gpuCandidate.packet = MakeOpaquePacket(
        scene, 0, 0, buffers, gpuCandidate.material,
        RenderMaterialMode::Opaque, RenderDrawFlags::None);
    RenderDrawItem pendingDirect = gpuCandidate;
    std::vector<RenderDrawItem> opaqueItems = {
        gpuCandidate, pendingDirect};
    std::vector<RenderDrawItem> maskedItems;

    SceneMeshPassPreparation preparation;
    OpaqueMeshPassProcessor processor;
    MeshPassProcessorInput candidateInput;
    candidateInput.packet = gpuCandidate.packet;
    candidateInput.sourceOrdinal = 0;
    preparation.opaque.Record(processor.Process(candidateInput));
    MeshPassProcessorInput directInput;
    directInput.packet = pendingDirect.packet;
    directInput.sourceOrdinal = 1;
    directInput.availability.specialMaterial = true;
    preparation.opaque.Record(processor.Process(directInput));
    preparation.depth.FinalizeGroups();
    preparation.opaque.FinalizeGroups();
    preparation.shadow.FinalizeGroups();
    preparation.transparent.FinalizeGroups();

    const RenderFramePlanCompileResult compiled = CompilePlan(
        preparation,
        RenderGPUDrivenMode::ForceEnabled,
        620,
        RenderPolicyReadiness::Pending);
    ASSERT_TRUE(compiled.succeeded);
    const RenderPassExecutionPlan& opaquePlan = compiled.plan.passes[1];
    EXPECT_EQ(1u, opaquePlan.partition.gpuDrivenPacketCount);
    EXPECT_EQ(0u, opaquePlan.partition.directPacketCount);
    EXPECT_EQ(1u, opaquePlan.partition.skippedPacketCount);
    EXPECT_EQ(RenderPolicyReason::ResourcesPending, opaquePlan.reason);
    EXPECT_EQ(1u,
              opaquePlan.reasonCounts[static_cast<size_t>(
                  RenderPolicyReason::None)]);
    EXPECT_EQ(1u,
              opaquePlan.reasonCounts[static_cast<size_t>(
                  RenderPolicyReason::ResourcesPending)]);
    EXPECT_TRUE(opaquePlan.identityAccounting.IsExactlyOnce());
    RenderFrameExecutionReport report = MakeExecutionReport(compiled.plan);

    GPUCulling culling;
    GPUCullingConfig cullingConfig;
    cullingConfig.maxInstances = 4;
    cullingConfig.enableOcclusionCulling = false;
    cullingConfig.enableDistanceCulling = false;
    culling.Initialize(&device, cullingConfig);
    culling.BeginFrame();
    GPUIndexedDrawDesc drawDesc;
    drawDesc.indexCount = buffers.submeshes[0].indexCount;
    drawDesc.firstIndex = buffers.submeshes[0].indexOffset;
    drawDesc.vertexOffset = buffers.submeshes[0].baseVertex;
    ASSERT_EQ(0u, culling.BeginDrawGroup(
        meshResource->GetId(),
        gpuCandidate.material.slot,
        MaterialPipelineVariant::Opaque,
        gpuCandidate.mesh,
        gpuCandidate.material));
    EXPECT_NE(RVX_INVALID_INDEX,
              culling.AddDrawItemInstance(
                  scene, gpuCandidate, drawDesc, 0));
    culling.EndDrawGroup();
    culling.EndFrame();
    culling.CullCpuFallback(view.viewMatrix, view.projectionMatrix);

    view.viewCache = &viewCache;
    view.renderFrameExecutionPlan = &compiled.plan;
    view.meshPassPreparation = &preparation;
    view.renderFrameExecutionReport = &report;

    OpaquePass pass;
    ConfigureResources(pass, gpuResources, pipelineCache, materialSystem);
    RecordingCommandContext ctx;
    ExecuteTypedOpaqueRecording(
        pass, view, opaqueItems, maskedItems, compiled.plan, preparation,
        report, colorView.Get(), nullptr, &culling, ctx);

    EXPECT_EQ(1u, ctx.beginRenderPassCount);
    EXPECT_EQ(1u, ctx.endRenderPassCount);
    EXPECT_EQ(1u, ctx.drawIndexedIndirectCount);
    EXPECT_EQ(0u, ctx.drawIndexedCount);
    EXPECT_FALSE(pass.GetDrawStats().directPacketPathUsed);
    EXPECT_EQ(RenderPolicyReason::None,
              pass.GetDrawStats().failureReason);

    const RenderPassExecutionReport* opaqueReport =
        FindPassExecutionReport(report, RenderPassKind::Opaque);
    ASSERT_NE(opaqueReport, nullptr);
    EXPECT_EQ(RenderExecutionStatus::Completed, opaqueReport->status);
    EXPECT_EQ(RenderExecutionStatus::Completed,
              opaqueReport->gpuDrivenLane.status);
    EXPECT_EQ(RenderExecutionStatus::NotAttempted,
              opaqueReport->directLane.status);
    EXPECT_EQ(1u, opaqueReport->gpuDrivenLane.executedPacketCount);
    EXPECT_EQ(1u, opaqueReport->skippedPacketCount);
    EXPECT_EQ(RenderPolicyReason::ResourcesPending,
              opaqueReport->reason);
}

TEST_F(RenderPassValidationFixture,
       OpaquePassRejectsPlannedInvalidSubmeshBeforeRecording)
{
    RVX_REQUIRE_RENDER_RUNTIME_PIPELINE();

    scene.GetMutableObject(0).entityId = 83;
    const MeshGPUBuffers buffers =
        gpuResources.GetMeshBuffers(meshResource->GetId());
    RenderDrawItem item = MakeDrawItem(MaterialRenderMode::Opaque);
    item.submeshIndex = static_cast<uint32>(buffers.submeshes.size());
    item.packet = MakeOpaquePacket(
        scene, 0, item.submeshIndex, buffers, item.material,
        RenderMaterialMode::Opaque, RenderDrawFlags::None);
    item.packet.arguments.indexCount = 3;
    std::vector<RenderDrawItem> opaqueItems = {item};
    std::vector<RenderDrawItem> maskedItems;
    SceneMeshPassPreparation preparation = PrepareOpaquePackets(
        opaqueItems, maskedItems);
    const RenderFramePlanCompileResult compiled =
        CompileForcedDirectPlan(preparation, 613);
    ASSERT_TRUE(compiled.succeeded);
    RenderFrameExecutionReport report = MakeExecutionReport(compiled.plan);
    view.viewCache = &viewCache;
    view.renderFrameExecutionPlan = &compiled.plan;
    view.meshPassPreparation = &preparation;
    view.renderFrameExecutionReport = &report;

    OpaquePass pass;
    ConfigureResources(pass, gpuResources, pipelineCache, materialSystem);
    RecordingCommandContext ctx;
    ExecuteTypedOpaqueRecording(
        pass, view, opaqueItems, maskedItems, compiled.plan, preparation,
        report, colorView.Get(), nullptr, nullptr, ctx);

    EXPECT_EQ(0u, ctx.beginRenderPassCount);
    EXPECT_EQ(0u, ctx.drawIndexedCount);
    EXPECT_EQ(RenderPolicyReason::UnexpectedRecordingFailure,
              pass.GetDrawStats().failureReason);
    const RenderPassExecutionReport* opaqueReport =
        FindPassExecutionReport(report, RenderPassKind::Opaque);
    ASSERT_NE(opaqueReport, nullptr);
    EXPECT_EQ(RenderExecutionStatus::Failed,
              opaqueReport->directLane.status);
}

TEST_F(RenderPassValidationFixture,
       OpaquePassRejectsPlannedBindingUploadFailuresBeforeRecording)
{
    RVX_REQUIRE_RENDER_RUNTIME_PIPELINE();

    scene.GetMutableObject(0).entityId = 84;
    const MeshGPUBuffers buffers =
        gpuResources.GetMeshBuffers(meshResource->GetId());
    RenderDrawItem item = MakeDrawItem(MaterialRenderMode::Opaque);
    item.packet = MakeOpaquePacket(
        scene, 0, 0, buffers, item.material,
        RenderMaterialMode::Opaque, RenderDrawFlags::None);
    std::vector<RenderDrawItem> opaqueItems = {item};
    std::vector<RenderDrawItem> maskedItems;
    SceneMeshPassPreparation preparation = PrepareOpaquePackets(
        opaqueItems, maskedItems);
    const RenderFramePlanCompileResult compiled =
        CompileForcedDirectPlan(preparation, 614);
    ASSERT_TRUE(compiled.succeeded);
    RenderFrameExecutionReport report = MakeExecutionReport(compiled.plan);
    view.viewCache = &viewCache;
    view.renderFrameExecutionPlan = &compiled.plan;
    view.meshPassPreparation = &preparation;
    view.renderFrameExecutionReport = &report;

    FakeBuffer* materialConstants =
        FindMutableCreatedBuffer(device, "MaterialConstantBuffer");
    ASSERT_NE(materialConstants, nullptr);
    materialConstants->SetMapSucceeds(false);

    OpaquePass pass;
    ConfigureResources(pass, gpuResources, pipelineCache, materialSystem);
    RecordingCommandContext ctx;
    ExecuteTypedOpaqueRecording(
        pass, view, opaqueItems, maskedItems, compiled.plan, preparation,
        report, colorView.Get(), nullptr, nullptr, ctx);

    EXPECT_EQ(0u, ctx.beginRenderPassCount);
    EXPECT_EQ(0u, ctx.drawIndexedCount);
    EXPECT_EQ(RenderPolicyReason::UnexpectedRecordingFailure,
              pass.GetDrawStats().failureReason);

    materialConstants->SetMapSucceeds(true);
    auto* objectConstants = static_cast<FakeBuffer*>(
        pipelineCache.m_objectConstantBuffer.Get());
    ASSERT_NE(objectConstants, nullptr);
    objectConstants->SetMapSucceeds(false);
    RenderFrameExecutionReport objectFailureReport =
        MakeExecutionReport(compiled.plan);
    view.renderFrameExecutionReport = &objectFailureReport;
    OpaquePass objectFailurePass;
    ConfigureResources(objectFailurePass,
                       gpuResources,
                       pipelineCache,
                       materialSystem);
    RecordingCommandContext objectFailureCtx;
    ExecuteTypedOpaqueRecording(
        objectFailurePass, view, opaqueItems, maskedItems, compiled.plan,
        preparation, objectFailureReport, colorView.Get(), nullptr, nullptr,
        objectFailureCtx);
    EXPECT_EQ(0u, objectFailureCtx.beginRenderPassCount);
    EXPECT_EQ(0u, objectFailureCtx.drawIndexedCount);
    EXPECT_EQ(RenderPolicyReason::UnexpectedRecordingFailure,
              objectFailurePass.GetDrawStats().failureReason);
}

TEST_F(RenderPassValidationFixture,
       OpaquePassUsesPublishedPacketsAfterDrawItemDrift)
{
    RVX_REQUIRE_RENDER_RUNTIME_PIPELINE();

    scene.GetMutableObject(0).entityId = 85;
    const MeshGPUBuffers buffers =
        gpuResources.GetMeshBuffers(meshResource->GetId());
    RenderDrawItem item = MakeDrawItem(MaterialRenderMode::Opaque);
    item.packet = MakeOpaquePacket(
        scene, 0, 0, buffers, item.material,
        RenderMaterialMode::Opaque, RenderDrawFlags::None);
    std::vector<RenderDrawItem> opaqueItems = {item};
    std::vector<RenderDrawItem> maskedItems;
    SceneMeshPassPreparation preparation = PrepareOpaquePackets(
        opaqueItems, maskedItems);
    const RenderFramePlanCompileResult compiled =
        CompileForcedDirectPlan(preparation, 615);
    ASSERT_TRUE(compiled.succeeded);
    RenderFrameExecutionReport report = MakeExecutionReport(compiled.plan);
    opaqueItems.front().renderMode = RenderMaterialMode::Masked;
    view.viewCache = &viewCache;
    view.renderFrameExecutionPlan = &compiled.plan;
    view.meshPassPreparation = &preparation;
    view.renderFrameExecutionReport = &report;

    OpaquePass pass;
    ConfigureResources(pass, gpuResources, pipelineCache, materialSystem);
    RecordingCommandContext ctx;
    ExecuteTypedOpaqueRecording(
        pass, view, opaqueItems, maskedItems, compiled.plan, preparation,
        report, colorView.Get(), nullptr, nullptr, ctx);

    EXPECT_EQ(1u, ctx.beginRenderPassCount);
    EXPECT_EQ(1u, ctx.endRenderPassCount);
    EXPECT_EQ(1u, ctx.drawIndexedCount);
    EXPECT_TRUE(pass.GetDrawStats().planValidated);
    EXPECT_TRUE(pass.GetDrawStats().directPacketPathUsed);
    EXPECT_EQ(RenderPolicyReason::None,
              pass.GetDrawStats().failureReason);
}

TEST_F(RenderPassValidationFixture,
       OpaquePassClearsForEmptyPlannedDirectRange)
{
    RVX_REQUIRE_RENDER_RUNTIME_PIPELINE();

    std::vector<RenderDrawItem> opaqueItems;
    std::vector<RenderDrawItem> maskedItems;
    SceneMeshPassPreparation preparation = PrepareOpaquePackets(
        opaqueItems, maskedItems);
    const RenderFramePlanCompileResult compiled =
        CompileForcedDirectPlan(preparation, 616);
    ASSERT_TRUE(compiled.succeeded);
    RenderFrameExecutionReport report = MakeExecutionReport(compiled.plan);
    view.viewCache = &viewCache;
    view.renderFrameExecutionPlan = &compiled.plan;
    view.meshPassPreparation = &preparation;
    view.renderFrameExecutionReport = &report;

    OpaquePass pass;
    ConfigureResources(pass, gpuResources, pipelineCache, materialSystem);
    RecordingCommandContext ctx;
    ExecuteTypedOpaqueRecording(
        pass, view, opaqueItems, maskedItems, compiled.plan, preparation,
        report, colorView.Get(), nullptr, nullptr, ctx);

    EXPECT_EQ(1u, ctx.beginRenderPassCount);
    EXPECT_EQ(1u, ctx.endRenderPassCount);
    EXPECT_EQ(0u, ctx.drawIndexedCount);
    EXPECT_TRUE(pass.GetDrawStats().directPacketPathUsed);
    EXPECT_TRUE(pass.GetDrawStats().planValidated);
    const RenderPassExecutionReport* opaqueReport =
        FindPassExecutionReport(report, RenderPassKind::Opaque);
    ASSERT_NE(opaqueReport, nullptr);
    EXPECT_EQ(RenderExecutionStatus::Completed,
              opaqueReport->directLane.status);
}

TEST_F(RenderPassValidationFixture,
       OpaquePassClearsNonEmptyAllSkipPlanWithoutExecutingDirectLane)
{
    RVX_REQUIRE_RENDER_RUNTIME_PIPELINE();

    scene.GetMutableObject(0).entityId = 89;
    const MeshGPUBuffers buffers =
        gpuResources.GetMeshBuffers(meshResource->GetId());
    ASSERT_TRUE(buffers.IsValid());
    RenderDrawItem item = MakeDrawItem(MaterialRenderMode::Opaque);
    item.packet = MakeOpaquePacket(
        scene, 0, 0, buffers, item.material,
        RenderMaterialMode::Opaque, RenderDrawFlags::None);
    std::vector<RenderDrawItem> opaqueItems = {item};
    std::vector<RenderDrawItem> maskedItems;
    SceneMeshPassPreparation preparation = PrepareOpaquePackets(
        opaqueItems, maskedItems);
    const RenderFramePlanCompileResult compiled = CompilePlan(
        preparation,
        RenderGPUDrivenMode::ForceDisabled,
        619,
        RenderPolicyReadiness::Pending);
    ASSERT_TRUE(compiled.succeeded);
    const auto opaquePlan = std::find_if(
        compiled.plan.passes.begin(),
        compiled.plan.passes.end(),
        [](const RenderPassExecutionPlan& passPlan)
        {
            return passPlan.pass == RenderPassKind::Opaque;
        });
    ASSERT_NE(opaquePlan, compiled.plan.passes.end());
    EXPECT_EQ(0u, opaquePlan->partition.gpuDrivenPacketCount);
    EXPECT_EQ(0u, opaquePlan->partition.directPacketCount);
    EXPECT_EQ(1u, opaquePlan->partition.skippedPacketCount);
    EXPECT_EQ(RenderPolicyReason::ResourcesPending, opaquePlan->reason);

    RenderFrameExecutionReport report = MakeExecutionReport(compiled.plan);
    view.viewCache = &viewCache;
    view.renderFrameExecutionPlan = &compiled.plan;
    view.meshPassPreparation = &preparation;
    view.renderFrameExecutionReport = &report;

    OpaquePass pass;
    ConfigureResources(pass, gpuResources, pipelineCache, materialSystem);
    RecordingCommandContext ctx;
    ExecuteTypedOpaqueRecording(
        pass, view, opaqueItems, maskedItems, compiled.plan, preparation,
        report, colorView.Get(), nullptr, nullptr, ctx);

    EXPECT_EQ(1u, ctx.beginRenderPassCount);
    EXPECT_EQ(1u, ctx.endRenderPassCount);
    EXPECT_EQ(0u, ctx.drawIndexedIndirectCount);
    EXPECT_EQ(0u, ctx.drawIndexedCount);
    EXPECT_TRUE(pass.GetDrawStats().planValidated);
    EXPECT_FALSE(pass.GetDrawStats().directPacketPathUsed);
    EXPECT_EQ(RenderPolicyReason::None,
              pass.GetDrawStats().failureReason);

    const RenderPassExecutionReport* opaqueReport =
        FindPassExecutionReport(report, RenderPassKind::Opaque);
    ASSERT_NE(opaqueReport, nullptr);
    EXPECT_EQ(RenderExecutionStatus::Completed, opaqueReport->status);
    EXPECT_EQ(RenderExecutionStatus::NotAttempted,
              opaqueReport->gpuDrivenLane.status);
    EXPECT_EQ(RenderExecutionStatus::NotAttempted,
              opaqueReport->directLane.status);
    EXPECT_EQ(1u, opaqueReport->skippedPacketCount);
    EXPECT_EQ(RenderPolicyReason::ResourcesPending,
              opaqueReport->reason);
}

TEST_F(RenderPassValidationFixture,
       OpaquePassDoesNotReplayDirectAfterPlannedGPULateFailure)
{
    RVX_REQUIRE_RENDER_RUNTIME_PIPELINE();

    scene.GetMutableObject(0).entityId = 86;
    scene.GetMutableObject(0).bounds = meshResource->GetBounds();
    const MeshGPUBuffers buffers =
        gpuResources.GetMeshBuffers(meshResource->GetId());
    RenderDrawItem item = MakeDrawItem(MaterialRenderMode::Opaque);
    item.packet = MakeOpaquePacket(
        scene, 0, 0, buffers, item.material,
        RenderMaterialMode::Opaque, RenderDrawFlags::None);
    std::vector<RenderDrawItem> opaqueItems = {item};
    std::vector<RenderDrawItem> maskedItems;
    SceneMeshPassPreparation preparation = PrepareOpaquePackets(
        opaqueItems, maskedItems);
    const RenderFramePlanCompileResult compiled =
        CompileForcedGPUPlan(preparation, 617);
    ASSERT_TRUE(compiled.succeeded);
    const auto opaquePlan = std::find_if(
        compiled.plan.passes.begin(),
        compiled.plan.passes.end(),
        [](const RenderPassExecutionPlan& pass)
        {
            return pass.pass == RenderPassKind::Opaque;
        });
    ASSERT_NE(opaquePlan, compiled.plan.passes.end());
    ASSERT_GT(opaquePlan->partition.gpuDrivenPacketCount, 0u);
    RenderFrameExecutionReport report = MakeExecutionReport(compiled.plan);

    GPUCulling culling;
    GPUCullingConfig cullingConfig;
    cullingConfig.maxInstances = 4;
    cullingConfig.enableOcclusionCulling = false;
    cullingConfig.enableDistanceCulling = false;
    culling.Initialize(&device, cullingConfig);
    ASSERT_NE(culling.GetInstanceBuffer(), nullptr);
    ASSERT_NE(culling.GetInstanceIndexBuffer(), nullptr);
    culling.BeginFrame();
    GPUIndexedDrawDesc drawDesc;
    drawDesc.indexCount = buffers.submeshes[0].indexCount;
    drawDesc.firstIndex = buffers.submeshes[0].indexOffset;
    drawDesc.vertexOffset = buffers.submeshes[0].baseVertex;
    ASSERT_EQ(0u, culling.BeginDrawGroup(meshResource->GetId()));
    EXPECT_NE(RVX_INVALID_INDEX,
              culling.AddDrawItemInstance(scene, item, drawDesc, 0));
    culling.EndDrawGroup();
    culling.EndFrame();
    culling.CullCpuFallback(view.viewMatrix, view.projectionMatrix);
    ASSERT_TRUE(culling.WasCpuFallbackUsedLastCull());

    auto* objectConstants = static_cast<FakeBuffer*>(
        pipelineCache.m_objectConstantBuffer.Get());
    ASSERT_NE(objectConstants, nullptr);
    objectConstants->SetMapSucceeds(false);
    view.viewCache = &viewCache;
    view.renderFrameExecutionPlan = &compiled.plan;
    view.meshPassPreparation = &preparation;
    view.renderFrameExecutionReport = &report;

    OpaquePass pass;
    ConfigureResources(pass, gpuResources, pipelineCache, materialSystem);
    RecordingCommandContext ctx;
    ExecuteTypedOpaqueRecording(
        pass, view, opaqueItems, maskedItems, compiled.plan, preparation,
        report, colorView.Get(), nullptr, &culling, ctx);

    EXPECT_EQ(1u, ctx.beginRenderPassCount);
    EXPECT_EQ(1u, ctx.endRenderPassCount);
    EXPECT_EQ(0u, ctx.drawIndexedCount);
    EXPECT_EQ(0u, ctx.drawIndexedIndirectCount);
    EXPECT_EQ(RenderPolicyReason::UnexpectedRecordingFailure,
              pass.GetDrawStats().failureReason);
    EXPECT_EQ(RenderExecutionStatus::Failed, report.status);
    const RenderPassExecutionReport* opaqueReport =
        FindPassExecutionReport(report, RenderPassKind::Opaque);
    ASSERT_NE(opaqueReport, nullptr);
    EXPECT_EQ(RenderExecutionStatus::Failed, opaqueReport->status);
    EXPECT_EQ(RenderExecutionStatus::Failed,
              opaqueReport->gpuDrivenLane.status);
    EXPECT_EQ(RenderExecutionStatus::NotAttempted,
              opaqueReport->directLane.status);
}

TEST_F(RenderPassValidationFixture,
       OpaquePassGPUSceneDependencyFailureSignalsFrameAbort)
{
    RVX_REQUIRE_RENDER_RUNTIME_PIPELINE();

    scene.GetMutableObject(0).entityId = 8601;
    scene.GetMutableObject(0).bounds = meshResource->GetBounds();
    const MeshGPUBuffers buffers =
        gpuResources.GetMeshBuffers(meshResource->GetId());
    ASSERT_TRUE(buffers.IsValid());
    ASSERT_FALSE(buffers.submeshes.empty());

    RenderDrawItem item = MakeDrawItem(MaterialRenderMode::Opaque);
    item.packet = MakeOpaquePacket(
        scene, 0, 0, buffers, item.material,
        RenderMaterialMode::Opaque, RenderDrawFlags::None);
    std::vector<RenderDrawItem> opaqueItems = {item};
    std::vector<RenderDrawItem> maskedItems;
    SceneMeshPassPreparation preparation = PrepareOpaquePackets(
        opaqueItems, maskedItems);
    const RenderFramePlanCompileResult compiled =
        CompileForcedGPUPlan(preparation, 8601);
    ASSERT_TRUE(compiled.succeeded);
    RenderFrameExecutionReport report = MakeExecutionReport(compiled.plan);

    GPUCulling culling;
    GPUCullingConfig cullingConfig;
    cullingConfig.maxInstances = 4;
    cullingConfig.enableOcclusionCulling = false;
    cullingConfig.enableDistanceCulling = false;
    culling.Initialize(&device, cullingConfig);
    culling.BeginFrame();
    GPUIndexedDrawDesc drawDesc;
    drawDesc.indexCount = buffers.submeshes[0].indexCount;
    drawDesc.firstIndex = buffers.submeshes[0].indexOffset;
    drawDesc.vertexOffset = buffers.submeshes[0].baseVertex;
    ASSERT_EQ(0u, culling.BeginDrawGroup(
        meshResource->GetId(), item.material.slot,
        MaterialPipelineVariant::Opaque, item.mesh, item.material));
    ASSERT_NE(RVX_INVALID_INDEX,
              culling.AddDrawItemInstance(scene, item, drawDesc, 0));
    culling.EndDrawGroup();
    culling.EndFrame();
    culling.CullCpuFallback(view.viewMatrix, view.projectionMatrix);

    RenderGraph graph;
    graph.SetDevice(&device);
    ViewData frameView = view;
    frameView.viewCache = &viewCache;
    frameView.colorTarget = graph.ImportTexture(
        colorTexture.Get(), RHIResourceState::RenderTarget);
    RenderPassRecordContext context = MakeMainSceneRecordContext(
        graph, frameView, scene, opaqueItems, maskedItems, compiled.plan,
        preparation, report, compiled.plan.frameSequence);
    context.opaqueGPUDriven = MakeSyntheticGPUSceneRasterInputs(
        graph, device, pipelineCache,
        MakeMainSceneGPUDrivenInputs(graph, context.identity, culling));
    ASSERT_TRUE(context.opaqueGPUDriven.IsCompatibleWith(context.identity));
    const std::shared_ptr<std::atomic_bool> recordingFailure =
        context.opaqueGPUDriven.gpuSceneRecordingFailure;

    OpaquePass pass;
    pass.SetResources(&pipelineCache, nullptr);
    pass.SetResourceRegistry(&gpuResources.GetRegistry());
    pass.AddToGraph(graph, context);
    graph.Compile();
    ASSERT_TRUE(graph.GetCompileStats().compileValid);
    RecordingCommandContext commands;
    graph.Execute(commands);
    pass.PublishRecordResults(context.results, context.identity);

    ASSERT_TRUE(recordingFailure);
    EXPECT_TRUE(recordingFailure->load());
    EXPECT_EQ(0u, commands.beginRenderPassCount);
    EXPECT_EQ(0u, commands.drawIndexedIndirectCount);
    EXPECT_EQ(RenderExecutionStatus::Failed,
              context.results->executionReport.status);
    const RenderPassExecutionReport* opaqueReport = FindPassExecutionReport(
        context.results->executionReport, RenderPassKind::Opaque);
    ASSERT_NE(opaqueReport, nullptr);
    EXPECT_EQ(RenderExecutionStatus::Failed, opaqueReport->status);
    EXPECT_EQ(RenderExecutionStatus::Failed,
              opaqueReport->gpuDrivenLane.status);
}

TEST_F(RenderPassValidationFixture, OpaquePassRejectsMalformedTypedRecordWithoutPlan)
{
    RVX_REQUIRE_RENDER_RUNTIME_PIPELINE();

    scene.GetMutableObject(0).bounds = meshResource->GetBounds();
    RenderDrawItem opaqueItem = MakeDrawItem(MaterialRenderMode::Opaque);
    std::vector<RenderDrawItem> opaqueItems = {opaqueItem};
    std::vector<RenderDrawItem> maskedItems;
    SceneMeshPassPreparation preparation = PrepareOpaquePackets(
        opaqueItems, maskedItems);
    const RenderFramePlanCompileResult compiled = CompileForcedGPUPlan(
        preparation, 621);
    ASSERT_TRUE(compiled.succeeded);
    RenderFrameExecutionReport report = MakeExecutionReport(compiled.plan);

    MeshGPUBuffers buffers = gpuResources.GetMeshBuffers(meshResource->GetId());
    ASSERT_TRUE(buffers.IsValid());
    ASSERT_FALSE(buffers.submeshes.empty());

    GPUIndexedDrawDesc drawDesc;
    drawDesc.indexCount = buffers.submeshes[0].indexCount;
    drawDesc.firstIndex = buffers.submeshes[0].indexOffset;
    drawDesc.vertexOffset = buffers.submeshes[0].baseVertex;

    GPUCulling culling;
    GPUCullingConfig cullingConfig;
    cullingConfig.maxInstances = 4;
    cullingConfig.enableOcclusionCulling = false;
    cullingConfig.enableDistanceCulling = false;
    culling.Initialize(&device, cullingConfig);
    culling.BeginFrame();
    ASSERT_EQ(0u, culling.BeginDrawGroup(
        meshResource->GetId(),
        opaqueItem.material.slot,
        MaterialPipelineVariant::Opaque,
        opaqueItem.mesh,
        opaqueItem.material));
    EXPECT_NE(RVX_INVALID_INDEX,
              culling.AddDrawItemInstance(scene, opaqueItem, drawDesc, 0));
    culling.EndDrawGroup();
    culling.EndFrame();
    culling.CullCpuFallback(view.viewMatrix, view.projectionMatrix);

    device.failGraphicsPipelineDebugName = "GPUDrivenOpaquePipeline";

    OpaquePass pass;
    ConfigureResources(pass, gpuResources, pipelineCache, materialSystem);
    RenderGraph graph;
    graph.SetDevice(&device);
    ViewData malformedView = view;
    malformedView.colorTarget = graph.ImportTexture(
        colorView->GetTexture(), RHIResourceState::RenderTarget);
    RenderPassRecordContext malformed = MakeMainSceneRecordContext(
        graph, malformedView, scene, opaqueItems, maskedItems, compiled.plan,
        preparation, report, compiled.plan.frameSequence);
    malformed.executionPlan = nullptr;
    malformed.view.renderFrameExecutionPlan = nullptr;
    pass.AddToGraph(graph, malformed);
    graph.Compile();
    RecordingCommandContext ctx;
    graph.Execute(ctx);

    EXPECT_EQ(0u, ctx.beginRenderPassCount);
    EXPECT_EQ(0u, ctx.drawIndexedCount);
    EXPECT_EQ(0u, ctx.drawIndexedIndirectCount);
}

TEST_F(RenderPassValidationFixture,
       OpaquePassRejectsMalformedTypedRecordForIncompleteGPUGroupCoverage)
{
    RVX_REQUIRE_RENDER_RUNTIME_PIPELINE();

    scene.GetMutableObject(0).bounds = meshResource->GetBounds();

    // The second source item represents a skinned/direct-only partition. It is
    // intentionally absent from the GPU culling groups below.
    auto directOnlyMeshResource = CreateTwoSubmeshMeshResource(402);
    directOnlyMeshResource->GetMesh()->SetBoneData(
        std::vector<IVec4>(6, IVec4(0, 0, 0, 0)),
        std::vector<Vec4>(6, Vec4(1.0f, 0.0f, 0.0f, 0.0f)));
    ASSERT_TRUE(gpuResources.UploadImmediate(directOnlyMeshResource.get()));
    ASSERT_TRUE(gpuResources.IsGPUReady(directOnlyMeshResource->GetId()));
    const MeshGPUBuffers directOnlyBuffers =
        gpuResources.GetMeshBuffers(directOnlyMeshResource->GetId());
    ASSERT_TRUE(directOnlyBuffers.IsValid());
    ASSERT_TRUE(directOnlyBuffers.HasSkinningVertexData());
    ASSERT_GE(directOnlyBuffers.submeshes.size(), 2u);

    RenderObject directOnlyObject = MakeRenderObject(*directOnlyMeshResource, gpuResources);
    directOnlyObject.skinningMatrices.push_back(Mat4Identity());
    scene.AddObject(directOnlyObject);
    ASSERT_TRUE(scene.GetObject(1).HasSkinningData());

    RenderDrawItem gpuCandidate = MakeDrawItem(MaterialRenderMode::Opaque);
    RenderDrawItem directOnly = MakeDrawItem(MaterialRenderMode::Opaque);
    directOnly.objectIndex = 1;
    directOnly.mesh = gpuResources.GetHandle(directOnlyMeshResource->GetId());
    directOnly.submeshIndex = 1;

    std::vector<RenderDrawItem> opaqueItems = {gpuCandidate, directOnly};
    std::vector<RenderDrawItem> maskedItems;
    SceneMeshPassPreparation preparation = PrepareOpaquePackets(
        opaqueItems, maskedItems);
    const RenderFramePlanCompileResult compiled = CompileForcedGPUPlan(
        preparation, 622);
    ASSERT_TRUE(compiled.succeeded);
    RenderFrameExecutionReport report = MakeExecutionReport(compiled.plan);

    MeshGPUBuffers candidateBuffers = gpuResources.GetMeshBuffers(meshResource->GetId());
    ASSERT_TRUE(candidateBuffers.IsValid());
    ASSERT_FALSE(candidateBuffers.submeshes.empty());

    GPUIndexedDrawDesc candidateDrawDesc;
    candidateDrawDesc.indexCount = candidateBuffers.submeshes[0].indexCount;
    candidateDrawDesc.firstIndex = candidateBuffers.submeshes[0].indexOffset;
    candidateDrawDesc.vertexOffset = candidateBuffers.submeshes[0].baseVertex;

    GPUCulling culling;
    GPUCullingConfig cullingConfig;
    cullingConfig.maxInstances = 4;
    cullingConfig.enableOcclusionCulling = false;
    cullingConfig.enableDistanceCulling = false;
    culling.Initialize(&device, cullingConfig);
    culling.BeginFrame();
    ASSERT_EQ(0u, culling.BeginDrawGroup(
        meshResource->GetId(),
        gpuCandidate.material.slot,
        MaterialPipelineVariant::Opaque,
        gpuCandidate.mesh,
        gpuCandidate.material));
    EXPECT_NE(RVX_INVALID_INDEX,
              culling.AddDrawItemInstance(scene, gpuCandidate, candidateDrawDesc, 0));
    culling.EndDrawGroup();
    culling.EndFrame();
    culling.CullCpuFallback(view.viewMatrix, view.projectionMatrix);

    ASSERT_EQ(1u, culling.GetDrawGroups().size());
    EXPECT_EQ(1u, culling.GetDrawGroups()[0].maxDrawCount);
    EXPECT_EQ(1u, culling.GetInstanceCount());

    OpaquePass pass;
    ConfigureResources(pass, gpuResources, pipelineCache, materialSystem);
    RenderGraph graph;
    graph.SetDevice(&device);
    ViewData malformedView = view;
    malformedView.colorTarget = graph.ImportTexture(
        colorView->GetTexture(), RHIResourceState::RenderTarget);
    RenderPassRecordContext malformed = MakeMainSceneRecordContext(
        graph, malformedView, scene, opaqueItems, maskedItems, compiled.plan,
        preparation, report, compiled.plan.frameSequence);
    malformed.executionPlan = nullptr;
    malformed.view.renderFrameExecutionPlan = nullptr;
    pass.AddToGraph(graph, malformed);
    graph.Compile();
    RecordingCommandContext ctx;
    graph.Execute(ctx);

    // Incomplete typed input has no valid plan and never replays Direct work.
    EXPECT_EQ(0u, ctx.beginRenderPassCount);
    EXPECT_EQ(0u, ctx.drawIndexedCount);
    EXPECT_EQ(0u, ctx.drawIndexedIndirectCount);
}

TEST_F(RenderPassValidationFixture, OpaquePassConsumesGPUDrivenMaterialGroupedIndirectStreams)
{
    RVX_REQUIRE_RENDER_RUNTIME_PIPELINE();

    scene.GetMutableObject(0).bounds = meshResource->GetBounds();

    RenderDrawItem opaqueItem = MakeDrawItem(MaterialRenderMode::Opaque);
    RenderDrawItem maskedItem = MakeDrawItem(MaterialRenderMode::Masked);
    std::vector<RenderDrawItem> opaqueItems = {opaqueItem};
    std::vector<RenderDrawItem> maskedItems = {maskedItem};

    MeshGPUBuffers buffers = gpuResources.GetMeshBuffers(meshResource->GetId());
    ASSERT_TRUE(buffers.IsValid());
    ASSERT_FALSE(buffers.submeshes.empty());

    opaqueItems[0].packet = MakeOpaquePacket(
        scene, 0, 0, buffers, opaqueItems[0].material,
        RenderMaterialMode::Opaque, RenderDrawFlags::None);
    maskedItems[0].packet = MakeOpaquePacket(
        scene, 0, 0, buffers, maskedItems[0].material,
        RenderMaterialMode::Masked, RenderDrawFlags::Masked);
    SceneMeshPassPreparation preparation = PrepareOpaquePackets(
        opaqueItems, maskedItems);
    const RenderFramePlanCompileResult compiled = CompileForcedGPUPlan(
        preparation, 623);
    ASSERT_TRUE(compiled.succeeded);
    RenderFrameExecutionReport report = MakeExecutionReport(compiled.plan);

    GPUIndexedDrawDesc drawDesc;
    drawDesc.indexCount = buffers.submeshes[0].indexCount;
    drawDesc.firstIndex = buffers.submeshes[0].indexOffset;
    drawDesc.vertexOffset = buffers.submeshes[0].baseVertex;

    GPUCulling culling;
    GPUCullingConfig cullingConfig;
    cullingConfig.maxInstances = 4;
    cullingConfig.enableOcclusionCulling = false;
    cullingConfig.enableDistanceCulling = false;
    culling.Initialize(&device, cullingConfig);
    culling.BeginFrame();

    ASSERT_EQ(0u, culling.BeginDrawGroup(
        meshResource->GetId(),
        opaqueItem.material.slot,
        MaterialPipelineVariant::Opaque,
        opaqueItem.mesh,
        opaqueItem.material));
    EXPECT_NE(RVX_INVALID_INDEX, culling.AddDrawItemInstance(scene, opaqueItem, drawDesc, 0));
    culling.EndDrawGroup();

    ASSERT_EQ(1u, culling.BeginDrawGroup(
        meshResource->GetId(),
        maskedItem.material.slot,
        MaterialPipelineVariant::Masked,
        maskedItem.mesh,
        maskedItem.material));
    EXPECT_NE(RVX_INVALID_INDEX, culling.AddDrawItemInstance(scene, maskedItem, drawDesc, 1));
    culling.EndDrawGroup();

    culling.EndFrame();
    culling.CullCpuFallback(view.viewMatrix, view.projectionMatrix);
    EXPECT_EQ(2u, culling.GetDrawCount());
    ASSERT_EQ(2u, culling.GetDrawGroups().size());

    OpaquePass pass;
    ConfigureResources(pass, gpuResources, pipelineCache, materialSystem);

    RecordingCommandContext ctx;
    ExecuteTypedOpaqueRecording(
        pass, view, opaqueItems, maskedItems, compiled.plan, preparation,
        report, colorView.Get(), nullptr, &culling, ctx);

    EXPECT_EQ(1u, ctx.beginRenderPassCount);
    EXPECT_EQ(1u, ctx.endRenderPassCount);
    EXPECT_EQ(0u, ctx.drawIndexedCount);
    EXPECT_EQ(2u, ctx.drawIndexedIndirectCount);
    EXPECT_EQ(1u, ctx.lastIndirectDrawCount);
    EXPECT_EQ(sizeof(IndirectDrawIndexedCommand), ctx.lastIndirectOffset);
    EXPECT_EQ(sizeof(IndirectDrawIndexedCommand), ctx.lastIndirectStride);
    ASSERT_EQ(static_cast<size_t>(2), ctx.pipelineSequence.size());
    ExpectPipelineDebugName(ctx.pipelineSequence[0], "GPUDrivenOpaquePipeline");
    ExpectPipelineDebugName(ctx.pipelineSequence[1], "GPUDrivenMaskedPipeline");

    const OpaquePassDrawStats& stats = pass.GetDrawStats();
    EXPECT_TRUE(stats.gpuDrivenRequested);
    EXPECT_TRUE(stats.gpuDrivenCullingReady);
    EXPECT_TRUE(stats.gpuDrivenPipelineReady);
    EXPECT_TRUE(stats.gpuDrivenEligible);
    EXPECT_TRUE(stats.gpuDrivenSubmitted);
    EXPECT_EQ(stats.gpuDrivenFallbackReason, GPUDrivenDrawFallbackReason::None);
    EXPECT_EQ(0u, stats.directDrawCount);
    EXPECT_EQ(2u, stats.gpuDrivenIndirectBatchCount);
    EXPECT_EQ(2u, stats.gpuDrivenIndirectDrawCount);
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
    EXPECT_NE(opaquePass.find("packet.materialKey.material, view.viewCache, materialOptions"),
              std::string::npos);

    EXPECT_NE(transparentPass.find("MaterialBindingOptions materialOptions;"), std::string::npos);
    EXPECT_NE(transparentPass.find("materialOptions.allowNormalMap = buffers.HasNormalMapTangentBasis()"),
              std::string::npos);
    EXPECT_NE(transparentPass.find("item.material, data.viewCache, materialOptions"),
              std::string::npos);
}

TEST_F(RenderPassValidationFixture, OpaqueAndTransparentPassBindFrameLightResources)
{
    RVX_REQUIRE_RENDER_RUNTIME_PIPELINE();

    const fs::path passesDir = FindShaderDirectory().parent_path() / "Private" / "Passes";
    const std::string opaquePass = ReadTextFile(passesDir / "OpaquePass.cpp");
    const std::string transparentPass = ReadTextFile(passesDir / "TransparentPass.cpp");

    EXPECT_NE(opaquePass.find("#include \"Render/Lighting/LightManager.h\""), std::string::npos);
    EXPECT_NE(opaquePass.find("#include \"Render/Lighting/ClusteredLighting.h\""), std::string::npos);
    EXPECT_NE(opaquePass.find("lightResources.lightConstantsBuffer = m_lightManager->GetLightConstantsBuffer();"),
              std::string::npos);
    EXPECT_NE(opaquePass.find("lightResources.pointLightsBuffer = m_lightManager->GetPointLightsBuffer();"),
              std::string::npos);
    EXPECT_NE(opaquePass.find("lightResources.spotLightsBuffer = m_lightManager->GetSpotLightsBuffer();"),
              std::string::npos);
    EXPECT_NE(opaquePass.find("lightResources.clusterConstantsBuffer = m_clusteredLighting->GetClusterConstantsBuffer();"),
              std::string::npos);
    EXPECT_NE(opaquePass.find("lightResources.clusterBuffer = m_clusteredLighting->GetClusterBuffer();"),
              std::string::npos);
    EXPECT_NE(opaquePass.find("lightResources.clusterLightIndexBuffer = m_clusteredLighting->GetLightIndexBuffer();"),
              std::string::npos);
    EXPECT_NE(opaquePass.find("planned.object.receivesShadow,"), std::string::npos);
    EXPECT_NE(opaquePass.find("m_pipelineCache->UpdateFrameLightResources(lightResources);"), std::string::npos);

    EXPECT_NE(transparentPass.find("#include \"Render/Lighting/LightManager.h\""), std::string::npos);
    EXPECT_NE(transparentPass.find("#include \"Render/Lighting/ClusteredLighting.h\""), std::string::npos);
    EXPECT_NE(transparentPass.find("m_lightManager->GetLightConstantsBuffer()"),
              std::string::npos);
    EXPECT_NE(transparentPass.find("m_lightManager->GetPointLightsBuffer()"),
              std::string::npos);
    EXPECT_NE(transparentPass.find("m_lightManager->GetSpotLightsBuffer()"),
              std::string::npos);
    EXPECT_NE(transparentPass.find("m_clusteredLighting->GetClusterConstantsBuffer()"),
              std::string::npos);
    EXPECT_NE(transparentPass.find("m_clusteredLighting->GetClusterBuffer()"),
              std::string::npos);
    EXPECT_NE(transparentPass.find("m_clusteredLighting->GetLightIndexBuffer()"),
              std::string::npos);
    EXPECT_NE(transparentPass.find("object.receivesShadow,"), std::string::npos);
    EXPECT_NE(transparentPass.find("CreateTransparentRasterDrawBindingSnapshot"),
              std::string::npos);
    EXPECT_EQ(transparentPass.find("UpdateDirectionalShadowFrameResources"),
              std::string::npos);
    EXPECT_EQ(transparentPass.find("UpdateFrameLightResources"), std::string::npos);
    EXPECT_EQ(transparentPass.find("UpdateViewConstants"), std::string::npos);
}

TEST_F(RenderPassValidationFixture, OpaquePassReportsShadowReceiverOptOutDrawItems)
{
    RVX_REQUIRE_RENDER_RUNTIME_PIPELINE();

    RenderObject& firstObject = scene.GetMutableObject(0);
    firstObject.receivesShadow = true;

    RenderObject secondObject = MakeRenderObject(*meshResource, gpuResources);
    secondObject.receivesShadow = false;
    scene.AddObject(secondObject);

    RenderDrawItem firstItem = MakeDrawItem(MaterialRenderMode::Opaque);
    RenderDrawItem secondItem = MakeDrawItem(MaterialRenderMode::Masked);
    secondItem.objectIndex = 1;

    std::vector<RenderDrawItem> opaqueItems = {firstItem};
    std::vector<RenderDrawItem> maskedItems = {secondItem};
    SceneMeshPassPreparation preparation = PrepareOpaquePackets(
        opaqueItems, maskedItems);
    const RenderFramePlanCompileResult compiled = CompileForcedDirectPlan(
        preparation, 640);
    ASSERT_TRUE(compiled.succeeded);
    RenderFrameExecutionReport report = MakeExecutionReport(compiled.plan);

    RenderGraph graph;
    graph.SetDevice(&device);
    view.colorTarget = graph.ImportTexture(colorTexture.Get(), RHIResourceState::RenderTarget);

    OpaquePass pass;
    pass.OnAdd(&device);
    ConfigureResources(pass, gpuResources, pipelineCache, materialSystem);
    RenderPassRecordContext context = MakeMainSceneRecordContext(
        graph, view, scene, opaqueItems, maskedItems, compiled.plan,
        preparation, report, compiled.plan.frameSequence);
    pass.AddToGraph(graph, context);
    graph.Compile();

    const OpaquePassShadowStats& stats = context.results->opaqueShadowStats;
    EXPECT_EQ(stats.receiverCandidateDrawItemCount, 2u);
    EXPECT_EQ(stats.shadowReceivingDrawItemCount, 1u);
    EXPECT_EQ(stats.shadowReceiverOptOutDrawItemCount, 1u);
}

TEST_F(RenderPassValidationFixture, OpaquePassResolvesRenderGraphColorTargetView)
{
    RVX_REQUIRE_RENDER_RUNTIME_PIPELINE();

    RenderGraph graph;
    graph.SetDevice(&device);

    RHITextureDesc sceneColorDesc = RHITextureDesc::RenderTarget(64, 64, RHIFormat::RGBA8_UNORM);
    sceneColorDesc.debugName = "GraphSceneColorForOpaquePass";
    view.colorTarget = graph.CreateTexture(sceneColorDesc);
    graph.SetExportState(view.colorTarget, RHIResourceState::RenderTarget);
    view.renderGraph = &graph;
    view.viewCache = &viewCache;

    scene.GetMutableObject(0).entityId = 641;
    const MeshGPUBuffers buffers =
        gpuResources.GetMeshBuffers(meshResource->GetId());
    ASSERT_TRUE(buffers.IsValid());
    ASSERT_FALSE(buffers.submeshes.empty());
    RenderDrawItem opaqueItem = MakeDrawItem(MaterialRenderMode::Opaque);
    opaqueItem.packet = MakeOpaquePacket(
        scene, 0, 0, buffers, opaqueItem.material,
        RenderMaterialMode::Opaque, RenderDrawFlags::None);
    std::vector<RenderDrawItem> opaqueItems = {opaqueItem};
    std::vector<RenderDrawItem> maskedItems;
    SceneMeshPassPreparation preparation = PrepareOpaquePackets(
        opaqueItems, maskedItems);
    const RenderFramePlanCompileResult compiled = CompileForcedDirectPlan(
        preparation, 641);
    ASSERT_TRUE(compiled.succeeded);
    RenderFrameExecutionReport report = MakeExecutionReport(compiled.plan);

    OpaquePass pass;
    ConfigureResources(pass, gpuResources, pipelineCache, materialSystem);
    RenderPassRecordContext context = MakeMainSceneRecordContext(
        graph, view, scene, opaqueItems, maskedItems, compiled.plan,
        preparation, report, compiled.plan.frameSequence);
    pass.AddToGraph(graph, context);
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

TEST_F(RenderPassValidationFixture,
       OpaquePassTypedRecordingOwnsGraphColorDepthAttachmentsUntilCompletion)
{
    RVX_REQUIRE_RENDER_RUNTIME_PIPELINE();
    device.SetFenceAutoComplete(false);

    const std::vector<RenderDrawItem> emptyDrawItems;
    const SceneMeshPassPreparation preparation = PrepareOpaquePackets(
        emptyDrawItems, emptyDrawItems);
    const RenderFramePlanCompileResult compiled = CompileForcedDirectPlan(
        preparation, 1701);
    ASSERT_TRUE(compiled.succeeded);

    struct OpaqueTargets
    {
        RHITextureRef color;
        RHITextureRef depth;
    };
    const auto makeContext = [this,
                              &compiled,
                              &preparation,
                              &emptyDrawItems](RenderGraph& graph,
                                               OpaqueTargets& targets,
                                               RenderSubmissionResourceBatch* batch,
                                               uint64 recordEpoch)
    {
        targets.color = device.CreateTexture(
            RHITextureDesc::RenderTarget(64, 64, RHIFormat::RGBA8_UNORM));
        targets.depth = device.CreateTexture(
            RHITextureDesc::DepthStencil(
                64, 64, PipelineCache::GetDefaultDepthStencilFormat()));
        EXPECT_TRUE(targets.color);
        EXPECT_TRUE(targets.depth);

        RenderPassRecordContext context;
        context.view = view;
        context.view.renderGraph = &graph;
        context.view.viewCache = &viewCache;
        context.view.submissionResourceBatch = batch;
        context.view.colorTarget = graph.ImportTexture(
            targets.color.Get(), RHIResourceState::RenderTarget);
        context.view.depthTarget = graph.ImportTexture(
            targets.depth.Get(), RHIResourceState::DepthWrite);
        context.view.viewportWidth = 64;
        context.view.viewportHeight = 64;
        context.view.renderFrameExecutionPlan = &compiled.plan;
        context.view.meshPassPreparation = &preparation;
        graph.SetExportState(context.view.colorTarget, RHIResourceState::RenderTarget);
        context.identity.graph = &graph;
        context.identity.graphIdentity = graph.GetGraphIdentity();
        context.identity.graphRecordingGeneration = graph.GetRecordingGeneration();
        context.identity.frameSequence = compiled.plan.frameSequence;
        context.identity.viewOrdinal = compiled.plan.viewOrdinal;
        context.identity.recordEpoch = recordEpoch;
        context.executionPlan = &compiled.plan;
        context.meshPassPreparation = &preparation;
        context.renderScene = &scene;
        context.opaqueDrawItems = &emptyDrawItems;
        context.maskedDrawItems = &emptyDrawItems;
        context.directionalShadow.identity = context.identity;
        context.rayTracedShadow.identity = context.identity;
        context.results = std::make_shared<RenderPassRecordResults>();
        context.frameSnapshot = MakeRenderPassFrameSnapshot(
            context, *context.results);
        return context;
    };

    const auto expectGraphOwnedAttachmentDeclarations = [](
                                                        const RenderGraph& graph,
                                                        const RenderPassRecordContext& context)
    {
        const RenderGraph::Diagnostics diagnostics = graph.GetDiagnostics();
        ASSERT_EQ(1u, diagnostics.passes.size());
        const auto passDiagnostic = std::find_if(
            diagnostics.passes.begin(), diagnostics.passes.end(),
            [](const RenderGraph::PassDiagnostic& diagnostic)
            {
                return diagnostic.name == "OpaquePass";
            });
        ASSERT_NE(diagnostics.passes.end(), passDiagnostic);
        const auto colorUsage = std::find_if(
            passDiagnostic->usages.begin(), passDiagnostic->usages.end(),
            [&context](const RenderGraph::ResourceUsageDiagnostic& usage)
            {
                return usage.type == RenderGraph::DiagnosticResourceType::Texture &&
                       usage.resourceIndex == context.view.colorTarget.index;
            });
        const auto depthUsage = std::find_if(
            passDiagnostic->usages.begin(), passDiagnostic->usages.end(),
            [&context](const RenderGraph::ResourceUsageDiagnostic& usage)
            {
                return usage.type == RenderGraph::DiagnosticResourceType::Texture &&
                       usage.resourceIndex == context.view.depthTarget.index;
            });
        ASSERT_NE(passDiagnostic->usages.end(), colorUsage);
        ASSERT_NE(passDiagnostic->usages.end(), depthUsage);
        EXPECT_EQ(RenderGraph::DiagnosticAccessType::Write, colorUsage->access);
        EXPECT_EQ(RHIResourceState::RenderTarget, colorUsage->desiredState);
        EXPECT_EQ(RenderGraph::DiagnosticAccessType::Write, depthUsage->access);
        EXPECT_EQ(RHIResourceState::DepthWrite, depthUsage->desiredState);
    };
    const auto expectNoOpaqueUsages = [](const RenderGraph& graph)
    {
        const RenderGraph::Diagnostics diagnostics = graph.GetDiagnostics();
        ASSERT_EQ(1u, diagnostics.passes.size());
        const auto passDiagnostic = std::find_if(
            diagnostics.passes.begin(), diagnostics.passes.end(),
            [](const RenderGraph::PassDiagnostic& diagnostic)
            {
                return diagnostic.name == "OpaquePass";
            });
        ASSERT_NE(diagnostics.passes.end(), passDiagnostic);
        EXPECT_TRUE(passDiagnostic->usages.empty());
    };

    RenderSubmissionTracker tracker;
    ASSERT_TRUE(tracker.Initialize(&device));
    RenderRetirementQueue retirement;
    ASSERT_TRUE(retirement.Initialize(&tracker));

    RHITextureRef rawDepth = device.CreateTexture(
        RHITextureDesc::DepthStencil(
            64, 64, PipelineCache::GetDefaultDepthStencilFormat()));
    RHITextureViewRef rawDepthView = device.CreateTextureView(rawDepth.Get());
    ASSERT_TRUE(rawDepth);
    ASSERT_TRUE(rawDepthView);

    RenderGraph graphA;
    RenderGraph graphB;
    graphA.SetDevice(&device);
    graphB.SetDevice(&device);
    RenderSubmissionResourceBatch batchA;
    RenderSubmissionResourceBatch batchB;
    OpaqueTargets targetsA;
    OpaqueTargets targetsB;
    RenderPassRecordContext contextA = makeContext(graphA, targetsA, &batchA, 1701);
    RenderPassRecordContext contextB = makeContext(graphB, targetsB, &batchB, 1702);

    OpaquePass pass;
    ConfigureResources(pass, gpuResources, pipelineCache, materialSystem);
    pass.AddToGraph(graphA, contextA);
    pass.AddToGraph(graphB, contextB);

    graphA.Compile();
    graphB.Compile();
    ASSERT_TRUE(graphA.GetCompileStats().compileValid);
    ASSERT_TRUE(graphB.GetCompileStats().compileValid);
    expectGraphOwnedAttachmentDeclarations(graphA, contextA);
    expectGraphOwnedAttachmentDeclarations(graphB, contextB);

    const uint32 retainedBeforeB = batchB.GetRetainedObjectCount();
    RecordingCommandContext commandsB;
    graphB.Execute(commandsB);
    ASSERT_EQ(1u, commandsB.renderPasses.size());
    ASSERT_EQ(1u, commandsB.renderPasses[0].colorAttachmentCount);
    EXPECT_EQ(targetsB.color.Get(),
              commandsB.renderPasses[0].colorAttachments[0].view->GetTexture());
    ASSERT_TRUE(commandsB.renderPasses[0].hasDepthStencil);
    EXPECT_EQ(targetsB.depth.Get(),
              commandsB.renderPasses[0].depthStencilAttachment.view->GetTexture());
    EXPECT_EQ(RHILoadOp::Clear,
              commandsB.renderPasses[0].depthStencilAttachment.depthLoadOp);
    EXPECT_EQ(RHIStoreOp::Store,
              commandsB.renderPasses[0].depthStencilAttachment.depthStoreOp);
    EXPECT_FALSE(commandsB.renderPasses[0].depthStencilAttachment.readOnly);
    // The frame descriptor plus both attachment views and parent textures.
    EXPECT_EQ(retainedBeforeB + 5u, batchB.GetRetainedObjectCount());

    const uint32 retainedBeforeA = batchA.GetRetainedObjectCount();
    RecordingCommandContext commandsA;
    graphA.Execute(commandsA);
    ASSERT_EQ(1u, commandsA.renderPasses.size());
    ASSERT_EQ(1u, commandsA.renderPasses[0].colorAttachmentCount);
    EXPECT_EQ(targetsA.color.Get(),
              commandsA.renderPasses[0].colorAttachments[0].view->GetTexture());
    ASSERT_TRUE(commandsA.renderPasses[0].hasDepthStencil);
    EXPECT_EQ(targetsA.depth.Get(),
              commandsA.renderPasses[0].depthStencilAttachment.view->GetTexture());
    EXPECT_EQ(RHILoadOp::Clear,
              commandsA.renderPasses[0].depthStencilAttachment.depthLoadOp);
    EXPECT_EQ(RHIStoreOp::Store,
              commandsA.renderPasses[0].depthStencilAttachment.depthStoreOp);
    EXPECT_FALSE(commandsA.renderPasses[0].depthStencilAttachment.readOnly);
    EXPECT_EQ(retainedBeforeA + 5u, batchA.GetRetainedObjectCount());

    RHITextureViewRef colorViewProbe(
        commandsA.renderPasses[0].colorAttachments[0].view);
    RHITextureViewRef depthViewProbe(
        commandsA.renderPasses[0].depthStencilAttachment.view);
    ASSERT_TRUE(colorViewProbe);
    ASSERT_TRUE(depthViewProbe);
    RHITextureRef colorTextureProbe(colorViewProbe->GetTexture());
    RHITextureRef depthTextureProbe(depthViewProbe->GetTexture());
    ASSERT_TRUE(colorTextureProbe);
    ASSERT_TRUE(depthTextureProbe);

    GPUCompletionToken completionB;
    GPUCompletionToken completionA;
    ASSERT_TRUE(InsertGPUCompletionPoint(completionB, tracker.Submit(&commandsB)));
    ASSERT_TRUE(InsertGPUCompletionPoint(completionA, tracker.Submit(&commandsA)));
    graphA.Clear();
    graphB.Clear();
    viewCache.Clear();
    targetsA.color.Reset();
    targetsA.depth.Reset();
    targetsB.color.Reset();
    targetsB.depth.Reset();
    EXPECT_EQ(2u, colorViewProbe->GetRefCount());
    EXPECT_EQ(2u, depthViewProbe->GetRefCount());
    EXPECT_EQ(2u, colorTextureProbe->GetRefCount());
    EXPECT_EQ(2u, depthTextureProbe->GetRefCount());

    batchB.SealAndTransfer(completionB, retirement);
    batchA.SealAndTransfer(completionA, retirement);
    ASSERT_GT(retirement.GetDiagnostics().entryCount, 0u);

    FakeFence* const graphicsFence =
        device.FindFenceWithSignal(completionA.points[0].value);
    ASSERT_NE(nullptr, graphicsFence);
    graphicsFence->Complete(completionB.points[0].value);
    EXPECT_EQ(GPUCompletionStatus::Pending, retirement.Poll());
    EXPECT_EQ(2u, colorViewProbe->GetRefCount());
    EXPECT_EQ(2u, depthViewProbe->GetRefCount());
    EXPECT_EQ(2u, colorTextureProbe->GetRefCount());
    EXPECT_EQ(2u, depthTextureProbe->GetRefCount());

    graphicsFence->Complete(completionA.points[0].value);
    EXPECT_EQ(GPUCompletionStatus::Completed, retirement.Poll());
    EXPECT_EQ(1u, colorViewProbe->GetRefCount());
    EXPECT_EQ(1u, depthViewProbe->GetRefCount());
    EXPECT_EQ(1u, colorTextureProbe->GetRefCount());
    EXPECT_EQ(1u, depthTextureProbe->GetRefCount());
    tracker.Shutdown();

    // A typed execution cannot fall back to compatibility views when its
    // record snapshot lacks the graph's ResourceViewCache.
    RenderGraph noCacheGraph;
    noCacheGraph.SetDevice(&device);
    OpaqueTargets noCacheTargets;
    RenderPassRecordContext noCache = makeContext(
        noCacheGraph, noCacheTargets, nullptr, 1703);
    auto noCacheSnapshot = std::make_shared<RenderPassFrameSnapshot>(
        *noCache.frameSnapshot);
    noCacheSnapshot->view.renderGraph = &noCacheGraph;
    noCacheSnapshot->view.viewCache = nullptr;
    noCacheSnapshot->view.renderFrameExecutionPlan =
        &noCacheSnapshot->executionPlan;
    noCacheSnapshot->view.meshPassPreparation =
        &noCacheSnapshot->meshPassPreparation;
    noCacheSnapshot->view.renderVisibility = &noCacheSnapshot->visibility;
    noCacheSnapshot->view.renderFrameExecutionReport =
        &noCache.results->executionReport;
    noCache.frameSnapshot = std::move(noCacheSnapshot);
    pass.AddToGraph(noCacheGraph, noCache);
    noCacheGraph.Compile();
    ASSERT_TRUE(noCacheGraph.GetCompileStats().compileValid);
    RecordingCommandContext noCacheCommands;
    noCacheGraph.Execute(noCacheCommands);
    EXPECT_EQ(0u, noCacheCommands.beginRenderPassCount);

    // Clear advances the graph recording generation. A stale context must not
    // bind a replacement target that reuses its old resource slot.
    RenderGraph staleGraph;
    staleGraph.SetDevice(&device);
    OpaqueTargets staleTargets;
    RenderPassRecordContext stale = makeContext(
        staleGraph, staleTargets, nullptr, 1704);
    stale.results->opaqueStats.directDrawCount = 83;
    stale.results->opaqueStats.failureReason = RenderPolicyReason::None;
    stale.results->executionReport.status = RenderExecutionStatus::Completed;
    stale.results->executionReport.frameSequence = stale.identity.frameSequence;
    ASSERT_TRUE(stale.MatchesTargetGraph(staleGraph));
    ASSERT_TRUE(stale.IsFrameIdentityValid());
    staleGraph.Clear();
    RHITextureRef replacementColor = device.CreateTexture(
        RHITextureDesc::RenderTarget(64, 64, RHIFormat::RGBA8_UNORM));
    ASSERT_TRUE(replacementColor);
    const RGTextureHandle replacementHandle = staleGraph.ImportTexture(
        replacementColor.Get(), RHIResourceState::RenderTarget);
    staleGraph.SetExportState(replacementHandle, RHIResourceState::RenderTarget);
    pass.AddToGraph(staleGraph, stale);
    staleGraph.Compile();
    ASSERT_TRUE(staleGraph.GetCompileStats().compileValid);
    expectNoOpaqueUsages(staleGraph);
    RecordingCommandContext staleCommands;
    staleGraph.Execute(staleCommands);
    EXPECT_EQ(0u, staleCommands.beginRenderPassCount);
    EXPECT_EQ(83u, stale.results->opaqueStats.directDrawCount);
    EXPECT_EQ(RenderPolicyReason::None, stale.results->opaqueStats.failureReason);
    EXPECT_EQ(RenderExecutionStatus::Completed, stale.results->executionReport.status);
    EXPECT_EQ(stale.identity.frameSequence,
              stale.results->executionReport.frameSequence);

    // Matching graph provenance is not enough: a current-generation handle
    // can still be forged beyond the graph resource table. It must not
    // declare usage, create views, or fall back to compatibility attachments.
    RenderGraph forgedGraph;
    forgedGraph.SetDevice(&device);
    OpaqueTargets forgedTargets;
    RenderPassRecordContext forged = makeContext(
        forgedGraph, forgedTargets, nullptr, 1705);
    RGTextureHandle forgedColor;
    forgedColor.index = 97;
    forgedColor.graphIdentity = forgedGraph.GetGraphIdentity();
    forgedColor.recordingGeneration = forgedGraph.GetRecordingGeneration();
    RGTextureHandle forgedDepth = forgedColor;
    forgedDepth.index = 98;
    forged.view.colorTarget = forgedColor;
    forged.view.depthTarget = forgedDepth;
    auto forgedSnapshot = std::make_shared<RenderPassFrameSnapshot>(
        *forged.frameSnapshot);
    forgedSnapshot->view.colorTarget = forgedColor;
    forgedSnapshot->view.depthTarget = forgedDepth;
    forgedSnapshot->view.renderGraph = &forgedGraph;
    forgedSnapshot->view.renderFrameExecutionPlan =
        &forgedSnapshot->executionPlan;
    forgedSnapshot->view.meshPassPreparation =
        &forgedSnapshot->meshPassPreparation;
    forgedSnapshot->view.renderVisibility = &forgedSnapshot->visibility;
    forgedSnapshot->view.renderFrameExecutionReport =
        &forged.results->executionReport;
    forged.frameSnapshot = std::move(forgedSnapshot);
    ASSERT_TRUE(forged.MatchesTargetGraph(forgedGraph));
    ASSERT_TRUE(forged.IsFrameIdentityValid());
    forged.results->opaqueStats.directDrawCount = 84;
    forged.results->opaqueStats.failureReason = RenderPolicyReason::None;
    forged.results->opaqueShadowStats.requested = true;
    forged.results->directionalShadowOutput.enabled = true;
    forged.results->directionalShadowOutput.shadowMapSize = 86;
    forged.results->executionReport.status = RenderExecutionStatus::Completed;
    forged.results->executionReport.frameSequence = forged.identity.frameSequence;
    pass.AddToGraph(forgedGraph, forged);
    forgedGraph.Compile();
    ASSERT_TRUE(forgedGraph.GetCompileStats().compileValid);
    expectNoOpaqueUsages(forgedGraph);
    RecordingCommandContext forgedCommands;
    forgedGraph.Execute(forgedCommands);
    EXPECT_EQ(0u, forgedCommands.beginRenderPassCount);
    EXPECT_EQ(84u, forged.results->opaqueStats.directDrawCount);
    EXPECT_EQ(RenderPolicyReason::None, forged.results->opaqueStats.failureReason);
    EXPECT_TRUE(forged.results->opaqueShadowStats.requested);
    EXPECT_TRUE(forged.results->directionalShadowOutput.enabled);
    EXPECT_EQ(86u, forged.results->directionalShadowOutput.shadowMapSize);
    EXPECT_EQ(RenderExecutionStatus::Completed, forged.results->executionReport.status);
    EXPECT_EQ(forged.identity.frameSequence,
              forged.results->executionReport.frameSequence);

    // A current record can carry stale snapshot attachments from the same
    // graph after Clear(). The replacement imports reuse their slots, so this
    // must be rejected by attachment provenance/description validation rather
    // than by the context, snapshot, or results ownership checks.
    RenderGraph reusedSlotGraph;
    reusedSlotGraph.SetDevice(&device);
    const RGTextureHandle staleColorHandle = reusedSlotGraph.CreateTexture(
        RHITextureDesc::RenderTarget(64, 64, RHIFormat::RGBA8_UNORM));
    const RGTextureHandle staleDepthHandle = reusedSlotGraph.CreateTexture(
        RHITextureDesc::DepthStencil(
            64, 64, PipelineCache::GetDefaultDepthStencilFormat()));
    reusedSlotGraph.Clear();
    OpaqueTargets reusedSlotTargets;
    RenderPassRecordContext reusedSlot = makeContext(
        reusedSlotGraph, reusedSlotTargets, nullptr, 1708);
    ASSERT_EQ(staleColorHandle.index, reusedSlot.view.colorTarget.index);
    ASSERT_EQ(staleDepthHandle.index, reusedSlot.view.depthTarget.index);
    ASSERT_EQ(staleColorHandle.graphIdentity, reusedSlot.identity.graphIdentity);
    ASSERT_NE(staleColorHandle.recordingGeneration,
              reusedSlot.identity.graphRecordingGeneration);
    ASSERT_EQ(reusedSlot.identity.graphRecordingGeneration,
              reusedSlot.view.colorTarget.recordingGeneration);
    ASSERT_EQ(reusedSlot.identity.graphRecordingGeneration,
              reusedSlot.view.depthTarget.recordingGeneration);
    auto reusedSlotSnapshot = std::make_shared<RenderPassFrameSnapshot>(
        *reusedSlot.frameSnapshot);
    reusedSlotSnapshot->view.colorTarget = staleColorHandle;
    reusedSlotSnapshot->view.depthTarget = staleDepthHandle;
    reusedSlotSnapshot->view.renderGraph = &reusedSlotGraph;
    reusedSlotSnapshot->view.renderFrameExecutionPlan =
        &reusedSlotSnapshot->executionPlan;
    reusedSlotSnapshot->view.meshPassPreparation =
        &reusedSlotSnapshot->meshPassPreparation;
    reusedSlotSnapshot->view.renderVisibility = &reusedSlotSnapshot->visibility;
    reusedSlotSnapshot->view.renderFrameExecutionReport =
        &reusedSlot.results->executionReport;
    reusedSlot.frameSnapshot = std::move(reusedSlotSnapshot);
    ASSERT_TRUE(reusedSlot.MatchesTargetGraph(reusedSlotGraph));
    ASSERT_TRUE(reusedSlot.IsFrameIdentityValid());
    reusedSlot.results->opaqueStats.directDrawCount = 85;
    reusedSlot.results->opaqueStats.failureReason = RenderPolicyReason::None;
    reusedSlot.results->opaqueShadowStats.requested = true;
    reusedSlot.results->directionalShadowOutput.enabled = true;
    reusedSlot.results->directionalShadowOutput.shadowMapSize = 87;
    reusedSlot.results->executionReport.status = RenderExecutionStatus::Completed;
    reusedSlot.results->executionReport.frameSequence =
        reusedSlot.identity.frameSequence;
    pass.AddToGraph(reusedSlotGraph, reusedSlot);
    reusedSlotGraph.Compile();
    ASSERT_TRUE(reusedSlotGraph.GetCompileStats().compileValid);
    expectNoOpaqueUsages(reusedSlotGraph);
    RecordingCommandContext reusedSlotCommands;
    reusedSlotGraph.Execute(reusedSlotCommands);
    EXPECT_EQ(0u, reusedSlotCommands.beginRenderPassCount);
    EXPECT_EQ(85u, reusedSlot.results->opaqueStats.directDrawCount);
    EXPECT_EQ(RenderPolicyReason::None,
              reusedSlot.results->opaqueStats.failureReason);
    EXPECT_TRUE(reusedSlot.results->opaqueShadowStats.requested);
    EXPECT_TRUE(reusedSlot.results->directionalShadowOutput.enabled);
    EXPECT_EQ(87u,
              reusedSlot.results->directionalShadowOutput.shadowMapSize);
    EXPECT_EQ(RenderExecutionStatus::Completed,
              reusedSlot.results->executionReport.status);
    EXPECT_EQ(reusedSlot.identity.frameSequence,
              reusedSlot.results->executionReport.frameSequence);

    // First resolve no cached views with creation disabled: the graph-owned
    // RTV failure must remain a fail-closed execution failure.
    viewCache.Clear();
    RenderGraph rtvFailureGraph;
    rtvFailureGraph.SetDevice(&device);
    OpaqueTargets rtvFailureTargets;
    RenderPassRecordContext rtvFailure = makeContext(
        rtvFailureGraph, rtvFailureTargets, nullptr, 1706);
    pass.AddToGraph(rtvFailureGraph, rtvFailure);
    rtvFailureGraph.Compile();
    ASSERT_TRUE(rtvFailureGraph.GetCompileStats().compileValid);
    expectGraphOwnedAttachmentDeclarations(rtvFailureGraph, rtvFailure);
    device.textureViewCreationSucceeds = false;
    RecordingCommandContext rtvFailureCommands;
    rtvFailureGraph.Execute(rtvFailureCommands);
    EXPECT_EQ(0u, rtvFailureCommands.beginRenderPassCount);
    device.textureViewCreationSucceeds = true;

    // Cache only the graph-owned RTV, then deny new view creation. This
    // isolates the DSV failure path from the previous RTV failure case.
    viewCache.Clear();
    RenderGraph dsvFailureGraph;
    dsvFailureGraph.SetDevice(&device);
    OpaqueTargets dsvFailureTargets;
    RenderPassRecordContext dsvFailure = makeContext(
        dsvFailureGraph, dsvFailureTargets, nullptr, 1707);
    ASSERT_NE(nullptr, viewCache.GetDefaultRTV(dsvFailureTargets.color.Get()));
    pass.AddToGraph(dsvFailureGraph, dsvFailure);
    dsvFailureGraph.Compile();
    ASSERT_TRUE(dsvFailureGraph.GetCompileStats().compileValid);
    expectGraphOwnedAttachmentDeclarations(dsvFailureGraph, dsvFailure);
    device.textureViewCreationSucceeds = false;
    RecordingCommandContext dsvFailureCommands;
    dsvFailureGraph.Execute(dsvFailureCommands);
    EXPECT_EQ(0u, dsvFailureCommands.beginRenderPassCount);
    device.textureViewCreationSucceeds = true;
}

TEST_F(RenderPassValidationFixture, OpaquePassDeclaresDirectionalShadowReadDuringSetup)
{
    RVX_REQUIRE_RENDER_RUNTIME_PIPELINE();

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
    shadowConfig.cascadeBlendRatio = 0.1f;

    ShadowPass shadowPass;
    ConfigureResources(shadowPass, gpuResources, pipelineCache);
    shadowPass.SetConfig(shadowConfig);
    shadowPass.SetEnabled(true);

    std::vector<RenderDrawItem> opaqueItems = {MakeDrawItem(MaterialRenderMode::Opaque)};
    std::vector<RenderDrawItem> maskedItems;
    SceneMeshPassPreparation preparation = PrepareOpaquePackets(
        opaqueItems, maskedItems);
    const RenderFramePlanCompileResult compiled = CompileForcedDirectPlan(
        preparation, 642);
    ASSERT_TRUE(compiled.succeeded);
    RenderFrameExecutionReport report = MakeExecutionReport(compiled.plan);

    OpaquePass opaquePass;
    ConfigureResources(opaquePass, gpuResources, pipelineCache, materialSystem);
    RenderPassRecordContext context = MakeMainSceneRecordContext(
        graph, view, scene, opaqueItems, maskedItems, compiled.plan,
        preparation, report, compiled.plan.frameSequence);
    context.primaryDirectionalLight = MakeShadowPrimaryLight();
    context.frameSnapshot = MakeRenderPassFrameSnapshot(
        context, *context.results);
    shadowPass.AddToGraph(graph, context);
    context.directionalShadow = context.results->directionalShadowOutput;
    ASSERT_EQ(context.directionalShadow.identity, context.identity);
    opaquePass.AddToGraph(graph, context);

    graph.Compile();
    const auto& stats = graph.GetCompileStats();
    EXPECT_TRUE(stats.compileValid);
    EXPECT_EQ(stats.totalPasses, 2u);
    EXPECT_EQ(stats.culledPasses, 0u);
    EXPECT_TRUE(context.results->opaqueShadowStats.requested);
    EXPECT_TRUE(context.results->opaqueShadowStats.renderGraphReadDeclared);

    // Opaque must draw from the snapshot captured at graph registration;
    // changing the producer before execution cannot alter its shadow inputs.
    shadowPass.SetEnabled(false);
    ShadowPassConfig mutatedShadowConfig = shadowPass.GetConfig();
    mutatedShadowConfig.shadowBias = 0.75f;
    mutatedShadowConfig.filterRadiusTexels = 9.0f;
    shadowPass.SetConfig(mutatedShadowConfig);

    RecordingCommandContext ctx;
    graph.Execute(ctx);

    shadowPass.PublishRecordResults(context.results, context.identity);
    opaquePass.PublishRecordResults(context.results, context.identity);
    report = context.results->executionReport;
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
    const auto& cascadeSplits =
        context.results->directionalShadowOutput.cascadeSplitDepths;
    ASSERT_EQ(cascadeSplits.size(), static_cast<size_t>(3));
    const float splitRange = view.farPlane - view.nearPlane;
    EXPECT_FLOAT_EQ(uploaded.directionalShadowCascadeSplits.x,
                    view.nearPlane + cascadeSplits[0] * splitRange);
    EXPECT_FLOAT_EQ(uploaded.directionalShadowCascadeSplits.y,
                    view.nearPlane + cascadeSplits[1] * splitRange);
    EXPECT_FLOAT_EQ(uploaded.directionalShadowCascadeSplits.z,
                    view.nearPlane + cascadeSplits[2] * splitRange);
    const float split0 = uploaded.directionalShadowCascadeSplits.x;
    const float split1 = uploaded.directionalShadowCascadeSplits.y;
    EXPECT_FLOAT_EQ(uploaded.directionalShadowCascadeFadeDistances.x,
                    (split0 - view.nearPlane) * shadowConfig.cascadeBlendRatio);
    EXPECT_FLOAT_EQ(uploaded.directionalShadowCascadeFadeDistances.y,
                    (split1 - split0) * shadowConfig.cascadeBlendRatio);
    EXPECT_FLOAT_EQ(uploaded.directionalShadowCascadeFadeDistances.z, 0.0f);

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
    RVX_REQUIRE_RENDER_RUNTIME_PIPELINE();

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
    shadowConfig.numCascades = 2;
    shadowConfig.shadowMapSize = 64;
    shadowConfig.cascadeBlendRatio = 0.1f;

    ShadowPass shadowPass;
    ConfigureResources(shadowPass, gpuResources, pipelineCache);
    shadowPass.SetConfig(shadowConfig);
    shadowPass.SetEnabled(true);

    std::vector<RenderDrawItem> opaqueItems = {MakeDrawItem(MaterialRenderMode::Opaque)};
    std::vector<RenderDrawItem> maskedItems;
    SceneMeshPassPreparation preparation = PrepareOpaquePackets(
        opaqueItems, maskedItems);
    const RenderFramePlanCompileResult compiled = CompileForcedDirectPlan(
        preparation, 643);
    ASSERT_TRUE(compiled.succeeded);
    RenderFrameExecutionReport report = MakeExecutionReport(compiled.plan);

    OpaquePass opaquePass;
    ConfigureResources(opaquePass, gpuResources, pipelineCache, materialSystem);
    RenderPassRecordContext context = MakeMainSceneRecordContext(
        graph, view, scene, opaqueItems, maskedItems, compiled.plan,
        preparation, report, compiled.plan.frameSequence);
    context.primaryDirectionalLight = MakeShadowPrimaryLight();
    context.frameSnapshot = MakeRenderPassFrameSnapshot(
        context, *context.results);
    shadowPass.AddToGraph(graph, context);
    context.directionalShadow = context.results->directionalShadowOutput;
    ASSERT_EQ(context.directionalShadow.identity, context.identity);
    opaquePass.AddToGraph(graph, context);

    graph.Compile();
    EXPECT_TRUE(context.results->opaqueShadowStats.renderGraphReadDeclared);

    device.failDirectionalShadowSRVCreation = true;
    RecordingCommandContext ctx;
    graph.Execute(ctx);

    shadowPass.PublishRecordResults(context.results, context.identity);
    opaquePass.PublishRecordResults(context.results, context.identity);
    report = context.results->executionReport;
    EXPECT_FALSE(opaquePass.GetShadowStats().frameShadowReady);
    const DirectionalShadowFrameBindingResult& binding =
        pipelineCache.GetLastDirectionalShadowFrameBindingResult();
    EXPECT_FALSE(binding.shadowSamplingEnabled);
    EXPECT_EQ(binding.fallbackReason, DirectionalShadowFallbackReason::MissingShadowSRV);

    const FakeBuffer* viewBuffer = FindCreatedBuffer(device, "ViewConstantBuffer");
    ASSERT_NE(viewBuffer, nullptr);
    ASSERT_GE(viewBuffer->GetStorage().size(), sizeof(ViewConstants));
    ViewConstants uploaded{};
    std::memcpy(&uploaded, viewBuffer->GetStorage().data(), sizeof(uploaded));
    EXPECT_FLOAT_EQ(uploaded.directionalShadowParams.x, 0.0f);
    EXPECT_FLOAT_EQ(uploaded.cameraForwardAndShadowCascadeCount.w, 0.0f);
    EXPECT_FLOAT_EQ(uploaded.directionalShadowCascadeSplits.x, 0.0f);
    EXPECT_FLOAT_EQ(uploaded.directionalShadowCascadeSplits.y, 0.0f);
    EXPECT_FLOAT_EQ(uploaded.directionalShadowCascadeFadeDistances.x, 0.0f);
    EXPECT_FLOAT_EQ(uploaded.directionalShadowCascadeFadeDistances.y, 0.0f);
}

TEST_F(RenderPassValidationFixture, TransparentPassBindsTransparentPipeline)
{
    RVX_REQUIRE_RENDER_RUNTIME_PIPELINE();

    std::vector<RenderDrawItem> transparentItems = {MakeDrawItem(MaterialRenderMode::Transparent)};

    RenderGraph graph;
    graph.SetDevice(&device);
    RHITextureRef colorTarget = device.CreateTexture(
        RHITextureDesc::RenderTarget(64, 64, RHIFormat::RGBA8_UNORM));
    ASSERT_TRUE(colorTarget);
    ViewData graphView = view;
    graphView.viewCache = &viewCache;
    graphView.viewportWidth = 64;
    graphView.viewportHeight = 64;
    graphView.colorTarget = graph.ImportTexture(
        colorTarget.Get(), RHIResourceState::RenderTarget);

    TransparentPass pass;
    ConfigureResources(pass, gpuResources, pipelineCache, materialSystem);
    RenderPassRecordContext context = MakeTransparentRecordContext(
        graph, graphView, scene, transparentItems, 1101, 1101);
    pass.AddToGraph(graph, context);
    graph.Compile();
    ASSERT_TRUE(graph.GetCompileStats().compileValid);

    RecordingCommandContext ctx;
    graph.Execute(ctx);

    ASSERT_EQ(static_cast<size_t>(1), ctx.pipelineSequence.size());
    EXPECT_NE(nullptr, ctx.pipelineSequence[0]);
    EXPECT_EQ(1u, ctx.drawIndexedCount);
    EXPECT_TRUE(std::any_of(ctx.descriptorSetSequence.begin(), ctx.descriptorSetSequence.end(),
                            [](uint32 set) { return set == 2; }));
}

TEST_F(RenderPassValidationFixture,
       TransparentPassTypedPathFailsClosedWhenRecordingBindingsCannotBeCreated)
{
    RVX_REQUIRE_RENDER_RUNTIME_PIPELINE(false);

    std::vector<RenderDrawItem> transparentItems = {MakeDrawItem(MaterialRenderMode::Transparent)};

    RenderGraph graph;
    graph.SetDevice(&device);
    RHITextureRef colorTarget = device.CreateTexture(
        RHITextureDesc::RenderTarget(64, 64, RHIFormat::RGBA8_UNORM));
    ASSERT_TRUE(colorTarget);
    ViewData graphView = view;
    graphView.viewCache = &viewCache;
    graphView.colorTarget = graph.ImportTexture(
        colorTarget.Get(), RHIResourceState::RenderTarget);

    TransparentPass pass;
    ConfigureResources(pass, gpuResources, pipelineCache, materialSystem);
    RenderPassRecordContext context = MakeTransparentRecordContext(
        graph, graphView, scene, transparentItems, 1102, 1102);
    pass.AddToGraph(graph, context);
    graph.Compile();
    ASSERT_TRUE(graph.GetCompileStats().compileValid);

    RecordingCommandContext ctx;
    graph.Execute(ctx);

    EXPECT_EQ(0u, ctx.drawIndexedCount);
    EXPECT_EQ(0u, ctx.beginRenderPassCount);
    EXPECT_FALSE(std::any_of(ctx.descriptorSetSequence.begin(), ctx.descriptorSetSequence.end(),
                             [](uint32 set) { return set == 2; }));
    const RenderGraph::Diagnostics diagnostics = graph.GetDiagnostics();
    const auto passDiagnostic = std::find_if(
        diagnostics.passes.begin(), diagnostics.passes.end(),
        [](const RenderGraph::PassDiagnostic& diagnostic)
        {
            return diagnostic.name == "TransparentPass";
        });
    ASSERT_NE(diagnostics.passes.end(), passDiagnostic);
    EXPECT_TRUE(passDiagnostic->usages.empty());
}

TEST_F(RenderPassValidationFixture, TransparentPassDrawsWhenMaterialBindingUsesFallback)
{
    RVX_REQUIRE_RENDER_RUNTIME_PIPELINE();

    Resource::TextureHandle missingTexture = CreateTextureResource(503);
    Resource::MaterialResource materialResource;
    ConfigureMaterialWithAlbedo(materialResource, missingTexture);

    RenderDrawItem item = MakeDrawItem(MaterialRenderMode::Transparent);
    item.material = gpuResources.ResolveOrUpload(&materialResource);
    std::vector<RenderDrawItem> transparentItems = {item};

    RenderGraph graph;
    graph.SetDevice(&device);
    RHITextureRef colorTarget = device.CreateTexture(
        RHITextureDesc::RenderTarget(64, 64, RHIFormat::RGBA8_UNORM));
    ASSERT_TRUE(colorTarget);
    ViewData graphView = view;
    graphView.viewCache = &viewCache;
    graphView.colorTarget = graph.ImportTexture(
        colorTarget.Get(), RHIResourceState::RenderTarget);

    TransparentPass pass;
    ConfigureResources(pass, gpuResources, pipelineCache, materialSystem);
    RenderPassRecordContext context = MakeTransparentRecordContext(
        graph, graphView, scene, transparentItems, 1103, 1103);
    pass.AddToGraph(graph, context);
    graph.Compile();
    ASSERT_TRUE(graph.GetCompileStats().compileValid);

    RecordingCommandContext ctx;
    graph.Execute(ctx);

    EXPECT_EQ(1u, ctx.drawIndexedCount);
    EXPECT_TRUE(std::any_of(ctx.descriptorSetSequence.begin(), ctx.descriptorSetSequence.end(),
                            [](uint32 set) { return set == 2; }));
}

TEST_F(RenderPassValidationFixture,
       TransparentPassOwnsReverseRecordingInputsAndSubmissionRetirement)
{
    RVX_REQUIRE_RENDER_RUNTIME_PIPELINE();
    device.SetFenceAutoComplete(false);

    RenderSubmissionTracker tracker;
    ASSERT_TRUE(tracker.Initialize(&device));
    RenderRetirementQueue retirement;
    ASSERT_TRUE(retirement.Initialize(&tracker));

    LightManager lightManager;
    lightManager.Initialize(&device);
    ASSERT_TRUE(lightManager.IsInitialized());
    ClusteringConfig clusterConfig;
    clusterConfig.clusterCountX = 1;
    clusterConfig.clusterCountY = 1;
    clusterConfig.clusterCountZ = 1;
    clusterConfig.maxLightsPerCluster = 1;
    ClusteredLighting clusteredLighting;
    ASSERT_TRUE(clusteredLighting.Initialize(&device, clusterConfig));

    const std::array<RHIBuffer*, 6> sourceBuffers = {
        lightManager.GetLightConstantsBuffer(),
        lightManager.GetPointLightsBuffer(),
        lightManager.GetSpotLightsBuffer(),
        clusteredLighting.GetClusterConstantsBuffer(),
        clusteredLighting.GetClusterBuffer(),
        clusteredLighting.GetLightIndexBuffer()};
    for (RHIBuffer* buffer : sourceBuffers)
    {
        ASSERT_NE(nullptr, buffer);
        ASSERT_GT(buffer->GetSize(), 0u);
        ASSERT_EQ(RHIMemoryType::Upload, buffer->GetMemoryType());
    }
    const auto fillSources = [&sourceBuffers](uint8 firstValue)
    {
        for (uint32 index = 0; index < sourceBuffers.size(); ++index)
        {
            void* mapped = sourceBuffers[index]->Map();
            ASSERT_NE(nullptr, mapped);
            std::memset(mapped,
                        static_cast<int>(firstValue + index),
                        static_cast<size_t>(sourceBuffers[index]->GetSize()));
            sourceBuffers[index]->Unmap();
        }
    };

    RenderObject objectA0 = scene.GetObject(0);
    objectA0.worldMatrix = Mat4Identity();
    objectA0.worldMatrix[3][0] = 2.0f;
    RenderObject objectA1 = objectA0;
    objectA1.worldMatrix[3][0] = 4.0f;
    RenderScene sceneA;
    sceneA.AddObject(objectA0);
    sceneA.AddObject(objectA1);
    RenderObject objectB = objectA0;
    objectB.worldMatrix[3][0] = 7.0f;
    RenderScene sceneB;
    sceneB.AddObject(objectB);

    RenderDrawItem transparentA0 = MakeDrawItem(MaterialRenderMode::Transparent);
    RenderDrawItem transparentA1 = transparentA0;
    transparentA1.objectIndex = 1;
    std::vector<RenderDrawItem> transparentItemsA = {transparentA0, transparentA1};
    std::vector<RenderDrawItem> transparentItemsB = {transparentA0};

    struct TransparentTargets
    {
        RHITextureRef color;
        RHITextureRef depth;
    };
    const auto makeContext = [&](RenderGraph& graph,
                                 const RenderScene& recordScene,
                                 const std::vector<RenderDrawItem>& drawItems,
                                 RenderSubmissionResourceBatch& batch,
                                 TransparentTargets& targets,
                                 uint64 sequence,
                                 float viewTranslation)
    {
        targets.color = device.CreateTexture(
            RHITextureDesc::RenderTarget(64, 64, RHIFormat::RGBA8_UNORM));
        targets.depth = device.CreateTexture(
            RHITextureDesc::DepthStencil(64, 64, RHIFormat::D32_FLOAT));
        EXPECT_TRUE(targets.color);
        EXPECT_TRUE(targets.depth);

        ViewData recordView = view;
        recordView.viewCache = &viewCache;
        recordView.submissionResourceBatch = &batch;
        recordView.colorTarget = graph.ImportTexture(
            targets.color.Get(), RHIResourceState::RenderTarget);
        recordView.depthTarget = graph.ImportTexture(
            targets.depth.Get(), RHIResourceState::DepthRead);
        recordView.viewProjectionMatrix = Mat4Identity();
        recordView.viewProjectionMatrix[3][0] = viewTranslation;
        recordView.directionalShadowViewProjection = Mat4Identity();
        recordView.directionalShadowViewProjection[3][2] = viewTranslation + 10.0f;
        recordView.directionalShadowViewProjections[0] =
            recordView.directionalShadowViewProjection;
        recordView.directionalShadowCascadeSplits = Vec4(5.0f, 11.0f, 0.0f, 0.0f);
        recordView.directionalShadowCascadeFadeDistances =
            Vec4(1.0f, 2.0f, 0.0f, 0.0f);
        recordView.directionalShadowCascadeCount = 2;
        recordView.directionalShadowDepthBias = 0.125f;
        recordView.directionalShadowStrength = 0.75f;
        recordView.directionalShadowInvMapSize = 0.25f;
        recordView.directionalShadowFilterRadiusTexels = 2.0f;
        recordView.directionalShadowNormalBias = 0.5f;
        recordView.directionalShadowEnabled = 1;
        recordView.rayTracedShadowEnabled = 1;
        recordView.rayTracedShadowFilterRadiusPixels = 2.0f;
        recordView.rayTracedShadowMode = RayTracedShadowMode::ReplaceRaster;
        return MakeTransparentRecordContext(
            graph, recordView, recordScene, drawItems, sequence, sequence, &batch);
    };

    RenderGraph graphA;
    RenderGraph graphB;
    graphA.SetDevice(&device);
    graphB.SetDevice(&device);
    RenderSubmissionResourceBatch batchA;
    RenderSubmissionResourceBatch batchB;
    TransparentTargets targetsA;
    TransparentTargets targetsB;
    RenderPassRecordContext contextA = makeContext(
        graphA, sceneA, transparentItemsA, batchA, targetsA, 1201, 3.0f);
    RenderPassRecordContext contextB = makeContext(
        graphB, sceneB, transparentItemsB, batchB, targetsB, 1202, 9.0f);

    TransparentPass pass;
    ConfigureResources(pass, gpuResources, pipelineCache, materialSystem);
    pass.SetResources(&pipelineCache, &materialSystem, &lightManager, &clusteredLighting);

    ASSERT_EQ(3u, pipelineCache.m_setLayouts.size());
    RHIPipelineLayoutRef pipelineLayoutProbe(pipelineCache.m_pipelineLayout.Get());
    ASSERT_TRUE(pipelineLayoutProbe);

    fillSources(0x10);
    pass.AddToGraph(graphA, contextA);
    fillSources(0x20);
    pass.AddToGraph(graphB, contextB);
    fillSources(0x30);

    // AddToGraph must snapshot its own shadow policy. It may disable shadow
    // sampling in the private upload, but never rewrite caller-owned view data.
    const auto expectCallerShadowInputs = [](const ViewData& recordView,
                                             float viewTranslation)
    {
        EXPECT_FLOAT_EQ(viewTranslation + 10.0f,
                        recordView.directionalShadowViewProjection[3][2]);
        EXPECT_EQ(2u, recordView.directionalShadowCascadeCount);
        EXPECT_FLOAT_EQ(0.125f, recordView.directionalShadowDepthBias);
        EXPECT_FLOAT_EQ(0.75f, recordView.directionalShadowStrength);
        EXPECT_FLOAT_EQ(0.25f, recordView.directionalShadowInvMapSize);
        EXPECT_FLOAT_EQ(2.0f, recordView.directionalShadowFilterRadiusTexels);
        EXPECT_FLOAT_EQ(0.5f, recordView.directionalShadowNormalBias);
        EXPECT_EQ(1u, recordView.directionalShadowEnabled);
        EXPECT_EQ(1u, recordView.rayTracedShadowEnabled);
        EXPECT_FLOAT_EQ(2.0f, recordView.rayTracedShadowFilterRadiusPixels);
        EXPECT_EQ(RayTracedShadowMode::ReplaceRaster,
                  recordView.rayTracedShadowMode);
    };
    expectCallerShadowInputs(contextA.view, 3.0f);
    expectCallerShadowInputs(contextB.view, 9.0f);

    const auto findRecordedBuffers = [this](const char* debugName)
    {
        std::vector<FakeBuffer*> buffers;
        for (size_t index = 0;
             index < device.createdBufferDescs.size() &&
             index < device.createdBuffers.size();
             ++index)
        {
            const char* name = device.createdBufferDescs[index].debugName;
            if (name != nullptr && std::string(name) == debugName)
            {
                buffers.push_back(device.createdBuffers[index]);
            }
        }
        return buffers;
    };
    constexpr std::array<const char*, 6> recordCopyNames = {
        "TransparentRecordLightConstants",
        "TransparentRecordPointLights",
        "TransparentRecordSpotLights",
        "TransparentRecordClusterConstants",
        "TransparentRecordClusterData",
        "TransparentRecordClusterLightIndices"};
    std::array<std::array<FakeBuffer*, 2>, 6> recordedLightCopies{};
    for (uint32 index = 0; index < recordCopyNames.size(); ++index)
    {
        const std::vector<FakeBuffer*> copies = findRecordedBuffers(recordCopyNames[index]);
        ASSERT_EQ(2u, copies.size()) << recordCopyNames[index];
        recordedLightCopies[index] = {copies[0], copies[1]};
        ASSERT_FALSE(copies[0]->GetStorage().empty());
        ASSERT_FALSE(copies[1]->GetStorage().empty());
        EXPECT_EQ(static_cast<uint8>(0x10 + index), copies[0]->GetStorage()[0]);
        EXPECT_EQ(static_cast<uint8>(0x20 + index), copies[1]->GetStorage()[0]);
        EXPECT_NE(copies[0], sourceBuffers[index]);
        EXPECT_NE(copies[1], sourceBuffers[index]);
    }

    const std::vector<FakeBuffer*> recordedViews =
        findRecordedBuffers("TransparentRecordViewConstants");
    const std::vector<FakeBuffer*> recordedObjects =
        findRecordedBuffers("TransparentRecordObjectConstants");
    ASSERT_EQ(2u, recordedViews.size());
    ASSERT_EQ(2u, recordedObjects.size());
    ViewConstants recordedViewA{};
    ViewConstants recordedViewB{};
    std::memcpy(&recordedViewA, recordedViews[0]->GetStorage().data(),
                sizeof(recordedViewA));
    std::memcpy(&recordedViewB, recordedViews[1]->GetStorage().data(),
                sizeof(recordedViewB));
    EXPECT_FLOAT_EQ(3.0f, recordedViewA.viewProjection[3][0]);
    EXPECT_FLOAT_EQ(9.0f, recordedViewB.viewProjection[3][0]);
    const auto expectPrivateShadowDisabled = [](const ViewConstants& constants)
    {
        EXPECT_FLOAT_EQ(0.0f, constants.cameraForwardAndShadowCascadeCount.w);
        EXPECT_FLOAT_EQ(0.0f, constants.directionalShadowParams.x);
        EXPECT_FLOAT_EQ(0.0f, constants.directionalShadowParams.y);
        EXPECT_FLOAT_EQ(0.0f, constants.directionalShadowParams.z);
        EXPECT_FLOAT_EQ(0.0f, constants.directionalShadowParams.w);
        EXPECT_FLOAT_EQ(0.0f, constants.directionalShadowReceiverParams.x);
        EXPECT_FLOAT_EQ(0.0f, constants.directionalShadowCascadeSplits.x);
        EXPECT_FLOAT_EQ(0.0f, constants.directionalShadowCascadeFadeDistances.x);
        EXPECT_FLOAT_EQ(0.0f, constants.rayTracedShadowParams.x);
        EXPECT_FLOAT_EQ(0.0f, constants.rayTracedShadowParams.y);
        EXPECT_FLOAT_EQ(0.0f, constants.rayTracedShadowParams.z);
    };
    expectPrivateShadowDisabled(recordedViewA);
    expectPrivateShadowDisabled(recordedViewB);

    const uint64 objectStride = pipelineCache.m_objectConstantStride;
    ASSERT_GT(objectStride, 0u);
    ObjectConstants recordedObjectA0{};
    ObjectConstants recordedObjectA1{};
    ObjectConstants recordedObjectB{};
    std::memcpy(&recordedObjectA0, recordedObjects[0]->GetStorage().data(),
                sizeof(recordedObjectA0));
    std::memcpy(&recordedObjectA1,
                recordedObjects[0]->GetStorage().data() + objectStride,
                sizeof(recordedObjectA1));
    std::memcpy(&recordedObjectB, recordedObjects[1]->GetStorage().data(),
                sizeof(recordedObjectB));
    EXPECT_FLOAT_EQ(2.0f, recordedObjectA0.world[3][0]);
    EXPECT_FLOAT_EQ(4.0f, recordedObjectA1.world[3][0]);
    EXPECT_FLOAT_EQ(7.0f, recordedObjectB.world[3][0]);

    // The captured descriptors expose the exact set0/set1 resources used by
    // each graph. Verify they are complete, private, and layout-ready rather
    // than merely observing that buffers with matching debug names exist.
    ASSERT_EQ(device.createdDescriptorSetDescs.size(),
              device.createdDescriptorSets.size());
    const auto findRecordedDescriptorSetIndices = [this](const char* debugName)
    {
        std::vector<size_t> indices;
        for (size_t index = 0; index < device.createdDescriptorSetDescs.size(); ++index)
        {
            const char* name = device.createdDescriptorSetDescs[index].debugName;
            if (name != nullptr && std::string(name) == debugName)
            {
                indices.push_back(index);
            }
        }
        return indices;
    };
    const std::vector<size_t> frameSetIndices =
        findRecordedDescriptorSetIndices("TransparentRecordFrameDescriptorSet");
    const std::vector<size_t> objectSetIndices =
        findRecordedDescriptorSetIndices("TransparentRecordObjectDescriptorSet");
    ASSERT_EQ(2u, frameSetIndices.size());
    ASSERT_EQ(2u, objectSetIndices.size());
    ASSERT_TRUE(pipelineCache.m_fallbackDirectionalShadowView);
    ASSERT_TRUE(pipelineCache.m_directionalShadowSampler);
    ASSERT_TRUE(pipelineCache.m_fallbackRayTracedShadowMaskView);

    const auto findBinding = [](const RHIDescriptorSetDesc& descriptor,
                                uint32 binding) -> const RHIDescriptorBinding*
    {
        const auto it = std::find_if(
            descriptor.bindings.begin(), descriptor.bindings.end(),
            [binding](const RHIDescriptorBinding& candidate)
            {
                return candidate.binding == binding && candidate.arrayElement == 0;
            });
        return it != descriptor.bindings.end() ? &(*it) : nullptr;
    };
    const auto expectBufferBinding = [&findBinding](
                                         const RHIDescriptorSetDesc& descriptor,
                                         uint32 binding,
                                         RHIBuffer* expectedBuffer)
    {
        const RHIDescriptorBinding* captured = findBinding(descriptor, binding);
        ASSERT_NE(nullptr, captured);
        EXPECT_EQ(expectedBuffer, captured->buffer);
        EXPECT_EQ(nullptr, captured->textureView);
        EXPECT_EQ(nullptr, captured->sampler);
    };
    const auto expectTextureBinding = [&findBinding](
                                          const RHIDescriptorSetDesc& descriptor,
                                          uint32 binding,
                                          RHITextureView* expectedView)
    {
        const RHIDescriptorBinding* captured = findBinding(descriptor, binding);
        ASSERT_NE(nullptr, captured);
        EXPECT_EQ(expectedView, captured->textureView);
        EXPECT_EQ(nullptr, captured->buffer);
        EXPECT_EQ(nullptr, captured->sampler);
    };
    const auto expectSamplerBinding = [&findBinding](
                                          const RHIDescriptorSetDesc& descriptor,
                                          uint32 binding,
                                          RHISampler* expectedSampler)
    {
        const RHIDescriptorBinding* captured = findBinding(descriptor, binding);
        ASSERT_NE(nullptr, captured);
        EXPECT_EQ(expectedSampler, captured->sampler);
        EXPECT_EQ(nullptr, captured->buffer);
        EXPECT_EQ(nullptr, captured->textureView);
    };
    const auto expectFrameDescriptor = [&](const RHIDescriptorSetDesc& descriptor,
                                            const FakeDescriptorSet* descriptorSet,
                                            FakeBuffer* expectedView,
                                            const std::array<FakeBuffer*, 6>& expectedCopies)
    {
        ASSERT_NE(nullptr, descriptorSet);
        EXPECT_EQ(pipelineCache.m_setLayouts[0].Get(), descriptor.layout);
        EXPECT_EQ(pipelineCache.m_setLayouts[0].Get(),
                  descriptorSet->GetLayoutIdentity());
        EXPECT_TRUE(descriptorSet->IsReadyForBinding(
            pipelineCache.m_setLayouts[0].Get()));
        EXPECT_EQ(descriptor.bindings.size(),
                  descriptorSet->GetDescriptorSnapshot().size());
        expectBufferBinding(descriptor, 0, expectedView);
        expectTextureBinding(descriptor, 1,
                             pipelineCache.m_fallbackDirectionalShadowView.Get());
        expectSamplerBinding(descriptor, 2,
                             pipelineCache.m_directionalShadowSampler.Get());
        expectBufferBinding(descriptor, 3, expectedCopies[0]);
        expectBufferBinding(descriptor, 4, expectedCopies[1]);
        expectBufferBinding(descriptor, 5, expectedCopies[2]);
        expectTextureBinding(descriptor, 6,
                             pipelineCache.m_fallbackRayTracedShadowMaskView.Get());
        expectBufferBinding(descriptor, 7, expectedCopies[3]);
        expectBufferBinding(descriptor, 8, expectedCopies[4]);
        expectBufferBinding(descriptor, 9, expectedCopies[5]);
    };

    const bool requiresObjectInstanceFallback =
        FindRHIBindingLayoutEntry(*pipelineCache.m_setLayouts[1], 1) != nullptr;
    const std::vector<FakeBuffer*> objectInstanceFallbacks =
        findRecordedBuffers("TransparentRecordObjectInstanceFallback");
    if (requiresObjectInstanceFallback)
    {
        ASSERT_EQ(2u, objectInstanceFallbacks.size());
    }
    else
    {
        EXPECT_TRUE(objectInstanceFallbacks.empty());
    }
    const auto expectObjectDescriptor = [&](const RHIDescriptorSetDesc& descriptor,
                                             const FakeDescriptorSet* descriptorSet,
                                             FakeBuffer* expectedObject,
                                             FakeBuffer* expectedInstanceFallback)
    {
        ASSERT_NE(nullptr, descriptorSet);
        EXPECT_EQ(pipelineCache.m_setLayouts[1].Get(), descriptor.layout);
        EXPECT_EQ(pipelineCache.m_setLayouts[1].Get(),
                  descriptorSet->GetLayoutIdentity());
        EXPECT_TRUE(descriptorSet->IsReadyForBinding(
            pipelineCache.m_setLayouts[1].Get()));
        EXPECT_EQ(descriptor.bindings.size(),
                  descriptorSet->GetDescriptorSnapshot().size());
        expectBufferBinding(descriptor, 0, expectedObject);
        const RHIDescriptorBinding* instanceBinding = findBinding(descriptor, 1);
        if (requiresObjectInstanceFallback)
        {
            ASSERT_NE(nullptr, instanceBinding);
            EXPECT_EQ(expectedInstanceFallback, instanceBinding->buffer);
        }
        else
        {
            EXPECT_EQ(nullptr, instanceBinding);
        }
    };
    expectFrameDescriptor(
        device.createdDescriptorSetDescs[frameSetIndices[0]],
        device.createdDescriptorSets[frameSetIndices[0]],
        recordedViews[0],
        {recordedLightCopies[0][0], recordedLightCopies[1][0],
         recordedLightCopies[2][0], recordedLightCopies[3][0],
         recordedLightCopies[4][0], recordedLightCopies[5][0]});
    expectFrameDescriptor(
        device.createdDescriptorSetDescs[frameSetIndices[1]],
        device.createdDescriptorSets[frameSetIndices[1]],
        recordedViews[1],
        {recordedLightCopies[0][1], recordedLightCopies[1][1],
         recordedLightCopies[2][1], recordedLightCopies[3][1],
         recordedLightCopies[4][1], recordedLightCopies[5][1]});
    expectObjectDescriptor(
        device.createdDescriptorSetDescs[objectSetIndices[0]],
        device.createdDescriptorSets[objectSetIndices[0]],
        recordedObjects[0],
        requiresObjectInstanceFallback ? objectInstanceFallbacks[0] : nullptr);
    expectObjectDescriptor(
        device.createdDescriptorSetDescs[objectSetIndices[1]],
        device.createdDescriptorSets[objectSetIndices[1]],
        recordedObjects[1],
        requiresObjectInstanceFallback ? objectInstanceFallbacks[1] : nullptr);

    // The recording is value-owned. Later frame extraction and source uploads
    // must not alter either graph's ordered draw/object/light input set.
    sceneA.GetMutableObject(0).worldMatrix[3][0] = 99.0f;
    transparentItemsA.clear();
    contextA.view.viewProjectionMatrix[3][0] = 99.0f;

    graphA.Compile();
    graphB.Compile();
    ASSERT_TRUE(graphA.GetCompileStats().compileValid);
    ASSERT_TRUE(graphB.GetCompileStats().compileValid);
    const RenderGraph::Diagnostics diagnosticsA = graphA.GetDiagnostics();
    const auto passA = std::find_if(
        diagnosticsA.passes.begin(), diagnosticsA.passes.end(),
        [](const RenderGraph::PassDiagnostic& diagnostic)
        {
            return diagnostic.name == "TransparentPass";
        });
    ASSERT_NE(diagnosticsA.passes.end(), passA);
    const auto colorUsage = std::find_if(
        passA->usages.begin(), passA->usages.end(),
        [&contextA](const RenderGraph::ResourceUsageDiagnostic& usage)
        {
            return usage.type == RenderGraph::DiagnosticResourceType::Texture &&
                   usage.resourceIndex == contextA.view.colorTarget.index;
        });
    const auto depthUsage = std::find_if(
        passA->usages.begin(), passA->usages.end(),
        [&contextA](const RenderGraph::ResourceUsageDiagnostic& usage)
        {
            return usage.type == RenderGraph::DiagnosticResourceType::Texture &&
                   usage.resourceIndex == contextA.view.depthTarget.index;
        });
    ASSERT_NE(passA->usages.end(), colorUsage);
    ASSERT_NE(passA->usages.end(), depthUsage);
    EXPECT_EQ(RenderGraph::DiagnosticAccessType::ReadWrite, colorUsage->access);
    EXPECT_EQ(RHIResourceState::RenderTarget, colorUsage->desiredState);
    EXPECT_EQ(RenderGraph::DiagnosticAccessType::Read, depthUsage->access);
    EXPECT_EQ(RHIResourceState::DepthRead, depthUsage->desiredState);

    const uint32 retainedBeforeB = batchB.GetRetainedObjectCount();
    RecordingCommandContext commandsB;
    graphB.Execute(commandsB);
    EXPECT_EQ(1u, commandsB.beginRenderPassCount);
    EXPECT_EQ(1u, commandsB.drawIndexedCount);
    EXPECT_GT(batchB.GetRetainedObjectCount(), retainedBeforeB);
    const uint32 retainedBeforeA = batchA.GetRetainedObjectCount();
    RecordingCommandContext commandsA;
    graphA.Execute(commandsA);
    EXPECT_EQ(1u, commandsA.beginRenderPassCount);
    EXPECT_EQ(2u, commandsA.drawIndexedCount);
    EXPECT_GT(batchA.GetRetainedObjectCount(), retainedBeforeA);
    ASSERT_EQ(1u, commandsA.renderPasses.size());
    ASSERT_EQ(1u, commandsB.renderPasses.size());
    EXPECT_EQ(targetsA.color.Get(),
              commandsA.renderPasses[0].colorAttachments[0].view->GetTexture());
    EXPECT_EQ(targetsB.color.Get(),
              commandsB.renderPasses[0].colorAttachments[0].view->GetTexture());
    ASSERT_TRUE(commandsA.renderPasses[0].hasDepthStencil);
    EXPECT_TRUE(commandsA.renderPasses[0].depthStencilAttachment.readOnly);

    const auto objectOffsets = [](const RecordingCommandContext& commands)
    {
        std::vector<uint32> offsets;
        for (size_t index = 0; index < commands.descriptorSetSequence.size(); ++index)
        {
            if (commands.descriptorSetSequence[index] == 1 &&
                commands.descriptorSetDynamicOffsets[index].size() == 1)
            {
                offsets.push_back(commands.descriptorSetDynamicOffsets[index][0]);
            }
        }
        return offsets;
    };
    EXPECT_EQ((std::vector<uint32>{0u, static_cast<uint32>(objectStride)}),
              objectOffsets(commandsA));
    EXPECT_EQ((std::vector<uint32>{0u}), objectOffsets(commandsB));

    GPUCompletionToken completionB;
    GPUCompletionToken completionA;
    ASSERT_TRUE(InsertGPUCompletionPoint(completionB, tracker.Submit(&commandsB)));
    ASSERT_TRUE(InsertGPUCompletionPoint(completionA, tracker.Submit(&commandsA)));
    batchB.SealAndTransfer(completionB, retirement);
    batchA.SealAndTransfer(completionA, retirement);
    EXPECT_TRUE(batchA.IsSealed());
    EXPECT_TRUE(batchB.IsSealed());
    EXPECT_EQ(0u, batchA.GetRetainedObjectCount());
    EXPECT_EQ(0u, batchB.GetRetainedObjectCount());
    ASSERT_GT(retirement.GetDiagnostics().entryCount, 0u);

    graphA.Clear();
    graphB.Clear();
    pipelineCache.Shutdown();
    EXPECT_EQ(3u, pipelineLayoutProbe->GetRefCount());
    FakeFence* const completionFence =
        device.FindFenceWithSignal(completionA.points[0].value);
    ASSERT_NE(nullptr, completionFence);
    completionFence->Complete(completionB.points[0].value);
    EXPECT_EQ(GPUCompletionStatus::Pending, retirement.Poll());
    EXPECT_EQ(2u, pipelineLayoutProbe->GetRefCount());
    completionFence->Complete(completionA.points[0].value);
    EXPECT_EQ(GPUCompletionStatus::Completed, retirement.Poll());
    EXPECT_EQ(1u, pipelineLayoutProbe->GetRefCount());
    clusteredLighting.Shutdown();
    lightManager.Shutdown();
    tracker.Shutdown();
}

TEST_F(RenderPassValidationFixture,
       TransparentPassFailsClosedForMalformedSealedAndEmptyRecordings)
{
    RVX_REQUIRE_RENDER_RUNTIME_PIPELINE();

    TransparentPass pass;
    ConfigureResources(pass, gpuResources, pipelineCache, materialSystem);
    std::vector<RenderDrawItem> transparentItems = {
        MakeDrawItem(MaterialRenderMode::Transparent)};
    const auto makeTargets = [this](uint32 extent)
    {
        return std::pair<RHITextureRef, RHITextureRef>(
            device.CreateTexture(
                RHITextureDesc::RenderTarget(extent, extent, RHIFormat::RGBA8_UNORM)),
            device.CreateTexture(
                RHITextureDesc::DepthStencil(extent, extent, RHIFormat::D32_FLOAT)));
    };
    const auto makeContext = [&](RenderGraph& graph,
                                 const RenderScene& recordScene,
                                 const std::vector<RenderDrawItem>& drawItems,
                                 RHITexture* color,
                                 RHITexture* depth,
                                 uint64 sequence,
                                 RenderSubmissionResourceBatch* batch = nullptr)
    {
        ViewData recordView = view;
        recordView.viewCache = &viewCache;
        recordView.colorTarget = graph.ImportTexture(color, RHIResourceState::RenderTarget);
        recordView.depthTarget = graph.ImportTexture(depth, RHIResourceState::DepthRead);
        return MakeTransparentRecordContext(
            graph, recordView, recordScene, drawItems, sequence, sequence, batch);
    };
    const auto expectNoUsageOrCommands = [](RenderGraph& graph,
                                            RecordingCommandContext& commands)
    {
        graph.Compile();
        ASSERT_TRUE(graph.GetCompileStats().compileValid);
        const RenderGraph::Diagnostics diagnostics = graph.GetDiagnostics();
        const auto passDiagnostic = std::find_if(
            diagnostics.passes.begin(), diagnostics.passes.end(),
            [](const RenderGraph::PassDiagnostic& diagnostic)
            {
                return diagnostic.name == "TransparentPass";
            });
        ASSERT_NE(diagnostics.passes.end(), passDiagnostic);
        EXPECT_TRUE(passDiagnostic->usages.empty());
        graph.Execute(commands);
        EXPECT_EQ(0u, commands.beginRenderPassCount);
        EXPECT_EQ(0u, commands.drawIndexedCount);
    };

    // A typed empty draw list is not an attachment-only pass.
    RenderGraph emptyGraph;
    emptyGraph.SetDevice(&device);
    auto emptyTargets = makeTargets(64);
    ASSERT_TRUE(emptyTargets.first);
    ASSERT_TRUE(emptyTargets.second);
    std::vector<RenderDrawItem> emptyItems;
    RenderPassRecordContext empty = makeContext(
        emptyGraph, scene, emptyItems, emptyTargets.first.Get(), emptyTargets.second.Get(), 1301);
    pass.AddToGraph(emptyGraph, empty);
    RecordingCommandContext emptyCommands;
    expectNoUsageOrCommands(emptyGraph, emptyCommands);

    // The ViewData overload cannot rebuild a typed mailbox.
    RenderGraph legacyGraph;
    legacyGraph.SetDevice(&device);
    auto legacyTargets = makeTargets(64);
    ASSERT_TRUE(legacyTargets.first);
    ASSERT_TRUE(legacyTargets.second);
    ViewData legacyView = view;
    legacyView.viewCache = &viewCache;
    legacyView.colorTarget = legacyGraph.ImportTexture(
        legacyTargets.first.Get(), RHIResourceState::RenderTarget);
    legacyView.depthTarget = legacyGraph.ImportTexture(
        legacyTargets.second.Get(), RHIResourceState::DepthRead);
    pass.AddToGraph(legacyGraph, legacyView);
    RecordingCommandContext legacyCommands;
    expectNoUsageOrCommands(legacyGraph, legacyCommands);

    // A foreign result object must not be initialized or overwritten before
    // the target graph has accepted the complete paired context.
    RenderGraph sourceGraph;
    RenderGraph targetGraph;
    sourceGraph.SetDevice(&device);
    targetGraph.SetDevice(&device);
    auto sourceTargets = makeTargets(64);
    ASSERT_TRUE(sourceTargets.first);
    ASSERT_TRUE(sourceTargets.second);
    RenderPassRecordContext foreign = makeContext(
        sourceGraph, scene, transparentItems,
        sourceTargets.first.Get(), sourceTargets.second.Get(), 1302);
    foreign.results->opaqueStats.directDrawCount = 791;
    foreign.frameSnapshot.reset();
    pass.AddToGraph(targetGraph, foreign);
    RecordingCommandContext foreignCommands;
    expectNoUsageOrCommands(targetGraph, foreignCommands);
    EXPECT_EQ(791u, foreign.results->opaqueStats.directDrawCount);
    EXPECT_EQ(foreign.identity, foreign.results->identity);

    // Stale and forged resource handles are rejected before retention or graph
    // access declarations, even when the remaining typed payload is paired.
    RenderGraph staleSourceGraph;
    RenderGraph staleGraph;
    staleSourceGraph.SetDevice(&device);
    staleGraph.SetDevice(&device);
    auto staleTargets = makeTargets(64);
    ASSERT_TRUE(staleTargets.first);
    ASSERT_TRUE(staleTargets.second);
    ViewData staleView = view;
    staleView.viewCache = &viewCache;
    staleView.colorTarget = staleSourceGraph.ImportTexture(
        staleTargets.first.Get(), RHIResourceState::RenderTarget);
    staleView.depthTarget = staleSourceGraph.ImportTexture(
        staleTargets.second.Get(), RHIResourceState::DepthRead);
    RenderPassRecordContext stale = MakeTransparentRecordContext(
        staleGraph, staleView, scene, transparentItems, 1303, 1303);
    pass.AddToGraph(staleGraph, stale);
    RecordingCommandContext staleCommands;
    expectNoUsageOrCommands(staleGraph, staleCommands);

    RenderGraph forgedGraph;
    forgedGraph.SetDevice(&device);
    RGTextureHandle forgedColor;
    forgedColor.index = 0;
    forgedColor.graphIdentity = forgedGraph.GetGraphIdentity();
    forgedColor.recordingGeneration = forgedGraph.GetRecordingGeneration();
    RGTextureHandle forgedDepth = forgedColor;
    forgedDepth.index = 1;
    ViewData forgedView = view;
    forgedView.viewCache = &viewCache;
    forgedView.colorTarget = forgedColor;
    forgedView.depthTarget = forgedDepth;
    RenderPassRecordContext forged = MakeTransparentRecordContext(
        forgedGraph, forgedView, scene, transparentItems, 1304, 1304);
    pass.AddToGraph(forgedGraph, forged);
    RecordingCommandContext forgedCommands;
    expectNoUsageOrCommands(forgedGraph, forgedCommands);

    // A self-graph incomplete snapshot and an already sealed submission batch
    // each fail before ReadWrite(color)/Read(depth) declarations become visible.
    RenderGraph partialGraph;
    partialGraph.SetDevice(&device);
    auto partialTargets = makeTargets(64);
    ASSERT_TRUE(partialTargets.first);
    ASSERT_TRUE(partialTargets.second);
    RenderPassRecordContext partial = makeContext(
        partialGraph, scene, transparentItems,
        partialTargets.first.Get(), partialTargets.second.Get(), 1305);
    partial.frameSnapshot.reset();
    pass.AddToGraph(partialGraph, partial);
    RecordingCommandContext partialCommands;
    expectNoUsageOrCommands(partialGraph, partialCommands);

    RenderSubmissionTracker tracker;
    ASSERT_TRUE(tracker.Initialize(&device));
    RenderRetirementQueue retirement;
    ASSERT_TRUE(retirement.Initialize(&tracker));
    RenderSubmissionResourceBatch sealedBatch;
    sealedBatch.ReleaseUnsubmitted(retirement);
    ASSERT_TRUE(sealedBatch.IsSealed());
    RenderGraph sealedGraph;
    sealedGraph.SetDevice(&device);
    auto sealedTargets = makeTargets(64);
    ASSERT_TRUE(sealedTargets.first);
    ASSERT_TRUE(sealedTargets.second);
    RenderPassRecordContext sealed = makeContext(
        sealedGraph, scene, transparentItems,
        sealedTargets.first.Get(), sealedTargets.second.Get(), 1306, &sealedBatch);
    pass.AddToGraph(sealedGraph, sealed);
    RecordingCommandContext sealedCommands;
    expectNoUsageOrCommands(sealedGraph, sealedCommands);
    EXPECT_EQ(GPUCompletionStatus::Completed, retirement.Poll());
    tracker.Shutdown();
}

TEST(RenderPassStatusValidation, ParticleFeaturePassStaysDisabledWithoutSnapshotItems)
{
    ParticleRenderSnapshot snapshot;
    snapshot.BeginBuild(3);
    snapshot.MarkComplete();

    ParticleFeaturePass pass;
    pass.SetSnapshot(&snapshot);

    const ParticleFeaturePassStats& stats = pass.GetStats();
    EXPECT_FALSE(stats.requested);
    EXPECT_FALSE(stats.supported);
    EXPECT_FALSE(stats.enabled);
    EXPECT_EQ(stats.itemCount, 0u);

    const RenderPassStatus status = pass.GetStatus();
    EXPECT_EQ("ParticleFeaturePass", status.name);
    EXPECT_FALSE(status.requestedEnabled);
    EXPECT_TRUE(status.supported);
    EXPECT_FALSE(status.enabled);
    EXPECT_TRUE(status.unsupportedReason.empty());
}

TEST(RenderPassStatusValidation, ParticleFeaturePassReportsMetadataOnlySnapshotsUnsupported)
{
    ParticleRenderSnapshot snapshot;
    snapshot.BeginBuild(7);

    ParticleRenderSnapshotItem item;
    item.instanceId = 42;
    item.systemId = 11;
    item.systemName = "CPU metadata-only particles";
    item.aliveParticleCount = 12;
    item.maxParticleCount = 64;
    item.payloadStatus = ParticleRenderSnapshotPayloadStatus::MetadataOnly;
    item.renderPayloadAvailable = false;
    item.sortingSupported = false;
    snapshot.items.push_back(item);
    snapshot.metadata.totalAliveParticles = 12;
    snapshot.MarkComplete();

    ParticleFeaturePass pass;
    pass.SetSnapshot(&snapshot);

    const ParticleFeaturePassStats& stats = pass.GetStats();
    EXPECT_TRUE(stats.requested);
    EXPECT_FALSE(stats.supported);
    EXPECT_FALSE(stats.enabled);
    EXPECT_EQ(stats.itemCount, 1u);
    EXPECT_EQ(stats.metadataOnlyItemCount, 1u);
    EXPECT_EQ(stats.renderPayloadReadyItemCount, 0u);
    EXPECT_EQ(stats.sortingSupportedItemCount, 0u);
    EXPECT_EQ(stats.totalAliveParticles, 12u);
    EXPECT_NE(stats.unsupportedReason.find("metadata-only"), std::string::npos);
    EXPECT_NE(stats.unsupportedReason.find("Render-owned"), std::string::npos);

    const RenderPassStatus status = pass.GetStatus();
    EXPECT_EQ("ParticleFeaturePass", status.name);
    EXPECT_TRUE(status.requestedEnabled);
    EXPECT_FALSE(status.supported);
    EXPECT_FALSE(status.enabled);
    EXPECT_EQ(status.unsupportedReason, stats.unsupportedReason);
}

TEST_F(RenderPassValidationFixture, RayTracingSceneManagerReportsStructuredFallbackCodes)
{
    RayTracingSceneBuildPlan emptyPlan;

    RayTracingSceneManager missingDeviceManager;
    EXPECT_FALSE(missingDeviceManager.Prepare(emptyPlan));
    EXPECT_EQ(missingDeviceManager.GetStats().fallbackCode,
              RayTracingSceneFallbackCode::MissingDevice);
    EXPECT_NE(std::string(missingDeviceManager.GetStats().fallbackReason).find("RHI device"),
              std::string::npos);

    FakeDevice unsupportedDevice;
    RayTracingSceneManager unsupportedManager;
    unsupportedManager.Initialize(&unsupportedDevice);
    EXPECT_FALSE(unsupportedManager.Prepare(emptyPlan));
    EXPECT_EQ(unsupportedManager.GetStats().fallbackCode,
              RayTracingSceneFallbackCode::RayTracingUnsupported);
    EXPECT_NE(std::string(unsupportedManager.GetStats().fallbackReason).find("does not support ray tracing"),
              std::string::npos);

    FakeDevice supportedDevice;
    supportedDevice.EnableRayTracing();
    RayTracingSceneManager emptyPlanManager;
    emptyPlanManager.Initialize(&supportedDevice);
    EXPECT_FALSE(emptyPlanManager.Prepare(emptyPlan));
    EXPECT_EQ(emptyPlanManager.GetStats().fallbackCode,
              RayTracingSceneFallbackCode::EmptyBuildPlan);
    EXPECT_NE(std::string(emptyPlanManager.GetStats().fallbackReason).find("no buildable work"),
              std::string::npos);
}

TEST(SceneRendererDiagnosticsValidation, ToolDiagnosticsSnapshotCarriesRHICapabilityReport)
{
    FakeDevice device;
    device.EnableBasicCapabilities();
    SceneRenderer renderer;
    renderer.SetRenderFeatureReportDeviceForTesting(&device);
    auto graph = std::make_unique<RenderGraph>();
    graph->SetDevice(&device);
    renderer.SetRenderGraphForTesting(std::move(graph));

    RHITextureDesc colorDesc = RHITextureDesc::RenderTarget(32, 24, RHIFormat::RGBA8_UNORM);
    colorDesc.usage = RHITextureUsage::RenderTarget | RHITextureUsage::ShaderResource;
    RHITextureRef colorTarget = device.CreateTexture(colorDesc);
    ASSERT_TRUE(colorTarget);

    SceneRendererExternalTargetDesc externalTarget;
    externalTarget.colorTarget = colorTarget.Get();
    externalTarget.colorInitialState = RHIResourceState::ShaderResource;
    externalTarget.colorFinalState = RHIResourceState::ShaderResource;
    renderer.SetExternalRenderTarget(externalTarget);

    renderer.BuildRenderGraphForTesting();

    const SceneRendererToolDiagnosticsSnapshot& toolSnapshot = renderer.GetToolDiagnosticsSnapshot();
    ASSERT_TRUE(toolSnapshot.rhiCapabilityReportAvailable);
    EXPECT_EQ(toolSnapshot.rhiCapabilityReport.schemaVersion, RVX_RHI_CAPABILITY_REPORT_SCHEMA_VERSION);
    EXPECT_EQ(toolSnapshot.rhiCapabilityReport.backendType, RHIBackendType::DX12);
    EXPECT_EQ(toolSnapshot.rhiCapabilityReport.adapterName, "RenderPassValidation Test Adapter");
    EXPECT_EQ(toolSnapshot.rhiCapabilityReport.driverVersion, "RenderPassValidation.Driver.1");
    EXPECT_TRUE(toolSnapshot.rhiCapabilityReport.validationPassed)
        << toolSnapshot.rhiCapabilityReport.validationMessage;
    EXPECT_TRUE(toolSnapshot.rhiCapabilityReport.renderGraphBaselineSupported);
    EXPECT_TRUE(toolSnapshot.rhiCapabilityReport.renderGraphBaselineMissingRequirements.empty());
    EXPECT_EQ(toolSnapshot.rhiCapabilityReport.entries.size(), static_cast<size_t>(12));

    const std::string diagnosticsText = renderer.ExportToolDiagnosticsText();
    EXPECT_NE(diagnosticsText.find("rhiCapabilities=true"), std::string::npos);
    EXPECT_NE(diagnosticsText.find("RHICapabilities: schema=5, backend=DirectX 12"), std::string::npos);
    EXPECT_NE(diagnosticsText.find("adapter=RenderPassValidation Test Adapter"), std::string::npos);
    EXPECT_NE(diagnosticsText.find("driver=RenderPassValidation.Driver.1"), std::string::npos);
    EXPECT_NE(diagnosticsText.find(
                  "queueCompletionMode=NativeTimeline, "
                  "logicalQueueDomains=[Graphics,Compute,Copy], activeDomainCount=3"),
              std::string::npos);
    EXPECT_NE(diagnosticsText.find("renderGraphBaseline=Passed"), std::string::npos);
    EXPECT_NE(diagnosticsText.find("RenderGraphBaselineMissing: none"), std::string::npos);
    EXPECT_NE(diagnosticsText.find("RHICapability ComputePipeline: status=Supported"), std::string::npos);

    const std::string manifestJson = renderer.ExportToolDiagnosticsManifestJson();
    EXPECT_NE(manifestJson.find("\"rhiCapabilityReportAvailable\": true"), std::string::npos);
    EXPECT_NE(manifestJson.find("\"rhiCapabilities\": {\n    \"schemaVersion\": 5"), std::string::npos);
    EXPECT_NE(manifestJson.find("\"backend\": \"DirectX 12\""), std::string::npos);
    EXPECT_NE(manifestJson.find("\"adapterName\": \"RenderPassValidation Test Adapter\""), std::string::npos);
    EXPECT_NE(manifestJson.find("\"driverVersion\": \"RenderPassValidation.Driver.1\""), std::string::npos);
    EXPECT_NE(manifestJson.find(
                  "\"queueTopology\": {\n"
                  "      \"completionMode\": \"NativeTimeline\",\n"
                  "      \"logicalQueueDomains\": [\"Graphics\", \"Compute\", \"Copy\"],\n"
                  "      \"activeDomainCount\": 3\n"
                  "    }"),
              std::string::npos);
    EXPECT_NE(manifestJson.find("\"renderGraphBaselineSupported\": true"), std::string::npos);
    EXPECT_NE(manifestJson.find("\"renderGraphBaselineMissingRequirements\": []"), std::string::npos);
    EXPECT_NE(manifestJson.find("\"feature\": \"ComputePipeline\""), std::string::npos);
    EXPECT_NE(manifestJson.find("\"status\": \"Supported\""), std::string::npos);

    const std::string rhiCapabilityJson = renderer.ExportToolRHICapabilityReportJson();
    EXPECT_NE(rhiCapabilityJson.find("\"schemaVersion\": 5"), std::string::npos);
    EXPECT_NE(rhiCapabilityJson.find("\"schemaId\": \"RVX.RHI.CapabilityReport\""), std::string::npos);
    EXPECT_NE(rhiCapabilityJson.find("\"id\": \"rhiCapabilityReportJson\""), std::string::npos);
    EXPECT_NE(rhiCapabilityJson.find("\"kind\": \"RHICapabilityReportJson\""), std::string::npos);
    EXPECT_NE(rhiCapabilityJson.find("\"contentType\": \"application/json\""), std::string::npos);
    EXPECT_NE(rhiCapabilityJson.find("\"contentHash\": \"\""), std::string::npos);
    EXPECT_NE(rhiCapabilityJson.find("\"relativePath\": \"\""), std::string::npos);
    EXPECT_NE(rhiCapabilityJson.find("\"adapterName\": \"RenderPassValidation Test Adapter\""),
              std::string::npos);
    EXPECT_NE(rhiCapabilityJson.find("\"driverVersion\": \"RenderPassValidation.Driver.1\""),
              std::string::npos);
    EXPECT_NE(rhiCapabilityJson.find("\"renderGraphBaseline\": {"), std::string::npos);
    EXPECT_NE(rhiCapabilityJson.find("\"supported\": true"), std::string::npos);
    EXPECT_FALSE(renderer.SaveToolRHICapabilityReportJson(nullptr));
    EXPECT_FALSE(renderer.SaveToolRHICapabilityReportJson(""));

    const auto suffix = std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    std::error_code removeError;
    const fs::path rhiCapabilityJsonPath =
        fs::temp_directory_path() / ("RVX_SceneRendererRHICapabilityReport_" + suffix + ".json");
    const std::string rhiCapabilityJsonPathString = rhiCapabilityJsonPath.string();
    ASSERT_TRUE(renderer.SaveToolRHICapabilityReportJson(rhiCapabilityJsonPathString.c_str()));
    EXPECT_EQ(ReadTextFile(rhiCapabilityJsonPath), rhiCapabilityJson);
    fs::remove(rhiCapabilityJsonPath, removeError);

    const fs::path artifactDirectory =
        fs::temp_directory_path() / ("RVX_SceneRendererRHICapabilityArtifacts_" + suffix);
    const std::string artifactDirectoryString = artifactDirectory.string();
    const SceneRendererToolDiagnosticsArtifactResult artifactResult =
        renderer.SaveToolDiagnosticsArtifacts(artifactDirectoryString.c_str(), "RHIFrame001");
    EXPECT_TRUE(artifactResult.rhiCapabilityReportJsonExpected);
    EXPECT_TRUE(artifactResult.rhiCapabilityReportJsonSaved);
    EXPECT_TRUE(artifactResult.rhiCapabilityReportJsonExists);
    EXPECT_EQ(artifactResult.rhiCapabilityReportSchemaId, RVX_RHI_CAPABILITY_REPORT_SCHEMA_ID);
    EXPECT_EQ(artifactResult.rhiCapabilityReportSchemaVersion, RVX_RHI_CAPABILITY_REPORT_SCHEMA_VERSION);
    EXPECT_EQ(artifactResult.primaryArtifactCount, 6u);
    EXPECT_EQ(artifactResult.savedPrimaryArtifactCount, 6u);
    EXPECT_EQ(artifactResult.artifactValidationCheckedPrimaryArtifactCount, 6u);
    EXPECT_EQ(artifactResult.artifactValidationValidPrimaryArtifactCount, 6u);
    EXPECT_EQ(artifactResult.artifactValidationFailedPrimaryArtifactCount, 0u);
    EXPECT_EQ(artifactResult.artifactValidationEntryCount, 6u);
    EXPECT_TRUE(artifactResult.artifactValidationEntryCountMatchesCheckedPrimaryArtifactCount);
    EXPECT_TRUE(artifactResult.allPrimaryArtifactsSaved);
    EXPECT_TRUE(artifactResult.artifactValidationAllPrimaryArtifactsValid);
    EXPECT_TRUE(artifactResult.artifactValidationBundleHashMatches);
    EXPECT_EQ(artifactResult.rhiCapabilityReportJsonContentHash.size(), 16u);
    EXPECT_GT(artifactResult.rhiCapabilityReportJsonBytes, 0u);
    EXPECT_NE(artifactResult.rhiCapabilityReportJsonPath.find("RHIFrame001.rhi-capabilities.json"),
              std::string::npos);
    EXPECT_EQ(artifactResult.rhiCapabilityReportJsonRelativePath, "RHIFrame001.rhi-capabilities.json");
    EXPECT_EQ(ReadTextFile(artifactResult.rhiCapabilityReportJsonPath), rhiCapabilityJson);

    const std::string artifactSummaryJson = ReadTextFile(artifactResult.artifactSummaryJsonPath);
    EXPECT_NE(artifactSummaryJson.find("\"artifactCount\": 6"), std::string::npos);
    EXPECT_NE(artifactSummaryJson.find("\"savedCount\": 6"), std::string::npos);
    EXPECT_NE(artifactSummaryJson.find("\"id\": \"rhiCapabilityReportJson\""), std::string::npos);
    EXPECT_NE(artifactSummaryJson.find("\"kind\": \"RHICapabilityReportJson\""), std::string::npos);
    EXPECT_NE(artifactSummaryJson.find("\"schemaId\": \"RVX.RHI.CapabilityReport\""), std::string::npos);
    EXPECT_NE(artifactSummaryJson.find("\"relativePath\": \"RHIFrame001.rhi-capabilities.json\""),
              std::string::npos);
    EXPECT_NE(artifactSummaryJson.find("\"checkedPrimaryArtifactCount\": 6"), std::string::npos);
    EXPECT_NE(artifactSummaryJson.find("\"validPrimaryArtifactCount\": 6"), std::string::npos);
    EXPECT_NE(artifactSummaryJson.find("\"count\": 6"), std::string::npos);

    const std::string artifactManifestJson = ReadTextFile(artifactResult.manifestJsonPath);
    EXPECT_NE(artifactManifestJson.find("\"primaryArtifactCount\": 6"), std::string::npos);
    EXPECT_NE(artifactManifestJson.find("\"savedPrimaryArtifactCount\": 6"), std::string::npos);
    EXPECT_NE(artifactManifestJson.find("\"rhiCapabilityReportJson\": {"), std::string::npos);
    EXPECT_NE(artifactManifestJson.find("\"kind\": \"RHICapabilityReportJson\""), std::string::npos);
    EXPECT_NE(artifactManifestJson.find("\"schemaId\": \"RVX.RHI.CapabilityReport\""), std::string::npos);
    EXPECT_NE(artifactManifestJson.find(
                  "\"schemaVersion\": " + std::to_string(RVX_RHI_CAPABILITY_REPORT_SCHEMA_VERSION)),
              std::string::npos);
    EXPECT_NE(artifactManifestJson.find("\"relativePath\": \"RHIFrame001.rhi-capabilities.json\""),
              std::string::npos);

    const SceneRendererToolDiagnosticsArtifactValidationResult validation =
        SceneRenderer::ValidateToolDiagnosticsArtifacts(artifactResult);
    EXPECT_TRUE(validation.allPrimaryArtifactsValid);
    EXPECT_EQ(validation.checkedPrimaryArtifactCount, 6u);
    EXPECT_EQ(validation.validPrimaryArtifactCount, 6u);
    EXPECT_EQ(validation.failedPrimaryArtifactCount, 0u);
    EXPECT_EQ(validation.entryCount, 6u);
    EXPECT_EQ(validation.rhiCapabilityReportSchemaId, RVX_RHI_CAPABILITY_REPORT_SCHEMA_ID);
    EXPECT_EQ(validation.rhiCapabilityReportSchemaVersion, RVX_RHI_CAPABILITY_REPORT_SCHEMA_VERSION);
    ASSERT_EQ(validation.diagnosticCodeCounts.size(), 1u);
    EXPECT_EQ(validation.diagnosticCodeCounts[0].code, "None");
    EXPECT_EQ(validation.diagnosticCodeCounts[0].count, 6u);
    bool rhiCapabilityJsonValidationFound = false;
    for (const SceneRendererToolDiagnosticsArtifactValidationEntry& entry : validation.entries)
    {
        if (entry.id == "rhiCapabilityReportJson")
        {
            rhiCapabilityJsonValidationFound = true;
            EXPECT_EQ(entry.entryIndex, 4u);
            EXPECT_EQ(entry.kind, "RHICapabilityReportJson");
            EXPECT_EQ(entry.contentType, "application/json");
            EXPECT_EQ(entry.schemaId, RVX_RHI_CAPABILITY_REPORT_SCHEMA_ID);
            EXPECT_EQ(entry.schemaVersion, RVX_RHI_CAPABILITY_REPORT_SCHEMA_VERSION);
            EXPECT_TRUE(entry.identityChecked);
            EXPECT_TRUE(entry.identityMatches);
            EXPECT_TRUE(entry.schemaChecked);
            EXPECT_TRUE(entry.schemaMatches);
            EXPECT_EQ(entry.actualId, "rhiCapabilityReportJson");
            EXPECT_EQ(entry.actualKind, "RHICapabilityReportJson");
            EXPECT_EQ(entry.actualContentType, "application/json");
            EXPECT_EQ(entry.actualSchemaId, RVX_RHI_CAPABILITY_REPORT_SCHEMA_ID);
            EXPECT_EQ(entry.actualSchemaVersion, RVX_RHI_CAPABILITY_REPORT_SCHEMA_VERSION);
            EXPECT_EQ(entry.relativePath, "RHIFrame001.rhi-capabilities.json");
            EXPECT_EQ(entry.expectedByteSize, artifactResult.rhiCapabilityReportJsonBytes);
            EXPECT_EQ(entry.actualByteSize, artifactResult.rhiCapabilityReportJsonBytes);
            EXPECT_EQ(entry.expectedContentHash, artifactResult.rhiCapabilityReportJsonContentHash);
            EXPECT_EQ(entry.actualContentHash, artifactResult.rhiCapabilityReportJsonContentHash);
        }
    }
    EXPECT_TRUE(rhiCapabilityJsonValidationFound);

    const std::string validationJson =
        SceneRenderer::ExportToolDiagnosticsArtifactValidationJson(validation);
    EXPECT_NE(validationJson.find("\"id\": \"rhiCapabilityReportJson\""), std::string::npos);
    EXPECT_NE(validationJson.find("\"kind\": \"RHICapabilityReportJson\""), std::string::npos);
    EXPECT_NE(validationJson.find("\"actualId\": \"rhiCapabilityReportJson\""), std::string::npos);
    EXPECT_NE(validationJson.find("\"actualSchemaId\": \"RVX.RHI.CapabilityReport\""),
              std::string::npos);
    EXPECT_NE(validationJson.find("\"entryCount\": 6"), std::string::npos);
    fs::remove_all(artifactDirectory, removeError);
}

TEST(SceneRendererDiagnosticsValidation, ToolDiagnosticsSnapshotIsVersionedAndCarriesRenderGraphState)
{
    FakeDevice device;
    SceneRenderer renderer;
    auto graph = std::make_unique<RenderGraph>();
    graph->SetDevice(&device);
    renderer.SetRenderGraphForTesting(std::move(graph));

    RHITextureDesc colorDesc = RHITextureDesc::RenderTarget(48, 32, RHIFormat::RGBA8_UNORM);
    colorDesc.usage = RHITextureUsage::RenderTarget | RHITextureUsage::ShaderResource;
    RHITextureRef colorTarget = device.CreateTexture(colorDesc);
    ASSERT_TRUE(colorTarget);

    SceneRendererExternalTargetDesc externalTarget;
    externalTarget.colorTarget = colorTarget.Get();
    externalTarget.colorInitialState = RHIResourceState::ShaderResource;
    externalTarget.colorFinalState = RHIResourceState::ShaderResource;
    renderer.SetExternalRenderTarget(externalTarget);

    renderer.BuildRenderGraphForTesting();

    const SceneRendererFrameDiagnostics& frameDiagnostics = renderer.GetFrameDiagnostics();
    const SceneRendererToolDiagnosticsSnapshot& toolSnapshot = renderer.GetToolDiagnosticsSnapshot();

    EXPECT_EQ(toolSnapshot.schemaVersion, RVX_SCENE_RENDERER_TOOL_DIAGNOSTICS_SCHEMA_VERSION);
    EXPECT_TRUE(toolSnapshot.frameDiagnosticsAvailable);
    EXPECT_EQ(toolSnapshot.frame.schemaVersion, RVX_SCENE_RENDERER_FRAME_DIAGNOSTICS_SCHEMA_VERSION);
    EXPECT_EQ(toolSnapshot.frame.frameCount, frameDiagnostics.frameCount);
    EXPECT_EQ(toolSnapshot.frame.graphBuilt, frameDiagnostics.graphBuilt);
    EXPECT_TRUE(toolSnapshot.renderGraphDiagnosticsAvailable);
    EXPECT_STREQ(toolSnapshot.renderGraph.schemaId, RVX_RENDER_GRAPH_DIAGNOSTICS_SCHEMA_ID);
    EXPECT_EQ(toolSnapshot.renderGraph.schemaVersion, RVX_RENDER_GRAPH_DIAGNOSTICS_SCHEMA_VERSION);
    EXPECT_FALSE(toolSnapshot.renderGraph.resources.empty());
    EXPECT_EQ(toolSnapshot.renderGraph.resources.size(), renderer.GetRenderGraph()->GetDiagnostics().resources.size());

    const std::string diagnosticsText = renderer.ExportToolDiagnosticsText();
    const std::string expectedSchemaText =
        "Schema: tool=" + std::to_string(RVX_SCENE_RENDERER_TOOL_DIAGNOSTICS_SCHEMA_VERSION) +
        ", frame=" + std::to_string(RVX_SCENE_RENDERER_FRAME_DIAGNOSTICS_SCHEMA_VERSION);
    EXPECT_NE(diagnosticsText.find("SceneRenderer Tool Diagnostics"), std::string::npos);
    EXPECT_NE(diagnosticsText.find(expectedSchemaText), std::string::npos);
    EXPECT_NE(diagnosticsText.find("Availability: frame=true, renderGraph=true"), std::string::npos);
    EXPECT_NE(diagnosticsText.find("ExternalTarget: requested=true, active=true"), std::string::npos);
    EXPECT_NE(diagnosticsText.find("qualification=Unqualified"), std::string::npos);
    EXPECT_NE(diagnosticsText.find(
                  "missingQualificationGates=RHIContractConformance"),
              std::string::npos);
    EXPECT_NE(diagnosticsText.find("RenderGraph: passes="), std::string::npos);
    EXPECT_NE(diagnosticsText.find("resources="), std::string::npos);
    EXPECT_FALSE(renderer.SaveToolDiagnosticsText(nullptr));
    EXPECT_FALSE(renderer.SaveToolDiagnosticsText(""));

    const auto suffix = std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const fs::path diagnosticsPath =
        fs::temp_directory_path() / ("RVX_SceneRendererToolDiagnostics_" + suffix + ".txt");
    const std::string diagnosticsPathString = diagnosticsPath.string();
    ASSERT_TRUE(renderer.SaveToolDiagnosticsText(diagnosticsPathString.c_str()));
    const std::string savedDiagnosticsText = ReadTextFile(diagnosticsPath);
    EXPECT_EQ(savedDiagnosticsText, diagnosticsText);
    std::error_code removeError;
    fs::remove(diagnosticsPath, removeError);

    const std::string graphviz = renderer.ExportToolRenderGraphGraphviz();
    EXPECT_NE(graphviz.find("digraph RenderGraph"), std::string::npos);
    EXPECT_NE(graphviz.find("cluster_resources"), std::string::npos);
    EXPECT_NE(graphviz.find("tex0"), std::string::npos);
    EXPECT_FALSE(renderer.SaveToolRenderGraphGraphviz(nullptr));
    EXPECT_FALSE(renderer.SaveToolRenderGraphGraphviz(""));

    const fs::path graphvizPath =
        fs::temp_directory_path() / ("RVX_SceneRendererToolRenderGraph_" + suffix + ".dot");
    const std::string graphvizPathString = graphvizPath.string();
    ASSERT_TRUE(renderer.SaveToolRenderGraphGraphviz(graphvizPathString.c_str()));
    const std::string savedGraphviz = ReadTextFile(graphvizPath);
    EXPECT_EQ(savedGraphviz, graphviz);
    fs::remove(graphvizPath, removeError);

    const std::string renderGraphDiagnosticsText = renderer.ExportToolRenderGraphDiagnosticsText();
    EXPECT_NE(renderGraphDiagnosticsText.find("RenderGraph Diagnostics"), std::string::npos);
    EXPECT_NE(renderGraphDiagnosticsText.find("Resources:"), std::string::npos);
    EXPECT_NE(renderGraphDiagnosticsText.find("Schedule efficiency:"), std::string::npos);
    EXPECT_FALSE(renderer.SaveToolRenderGraphDiagnosticsText(nullptr));
    EXPECT_FALSE(renderer.SaveToolRenderGraphDiagnosticsText(""));

    const fs::path renderGraphDiagnosticsPath =
        fs::temp_directory_path() / ("RVX_SceneRendererToolRenderGraphDiagnostics_" + suffix + ".txt");
    const std::string renderGraphDiagnosticsPathString = renderGraphDiagnosticsPath.string();
    ASSERT_TRUE(renderer.SaveToolRenderGraphDiagnosticsText(renderGraphDiagnosticsPathString.c_str()));
    const std::string savedRenderGraphDiagnostics = ReadTextFile(renderGraphDiagnosticsPath);
    EXPECT_EQ(savedRenderGraphDiagnostics, renderGraphDiagnosticsText);
    fs::remove(renderGraphDiagnosticsPath, removeError);

    const std::string renderGraphDiagnosticsJson = renderer.ExportToolRenderGraphDiagnosticsJson();
    EXPECT_NE(renderGraphDiagnosticsJson.find(
                  "\"schemaVersion\": " + std::to_string(RVX_RENDER_GRAPH_DIAGNOSTICS_SCHEMA_VERSION)),
              std::string::npos);
    EXPECT_NE(renderGraphDiagnosticsJson.find("\"schemaId\": \"RVX.RenderGraph.Diagnostics\""),
              std::string::npos);
    EXPECT_NE(renderGraphDiagnosticsJson.find("\"id\": \"renderGraphDiagnosticsJson\""), std::string::npos);
    EXPECT_NE(renderGraphDiagnosticsJson.find("\"kind\": \"RenderGraphDiagnosticsJson\""), std::string::npos);
    EXPECT_NE(renderGraphDiagnosticsJson.find("\"contentType\": \"application/json\""), std::string::npos);
    EXPECT_NE(renderGraphDiagnosticsJson.find("\"contentHash\": \"\""), std::string::npos);
    EXPECT_NE(renderGraphDiagnosticsJson.find("\"relativePath\": \"\""), std::string::npos);
    EXPECT_NE(renderGraphDiagnosticsJson.find("\"compileStats\": {"), std::string::npos);
    EXPECT_NE(renderGraphDiagnosticsJson.find("\"passes\": ["), std::string::npos);
    EXPECT_NE(renderGraphDiagnosticsJson.find("\"resources\": ["), std::string::npos);
    EXPECT_NE(renderGraphDiagnosticsJson.find("\"schedule\": {"), std::string::npos);
    EXPECT_NE(renderGraphDiagnosticsJson.find("\"name\": "), std::string::npos);
    EXPECT_FALSE(renderer.SaveToolRenderGraphDiagnosticsJson(nullptr));
    EXPECT_FALSE(renderer.SaveToolRenderGraphDiagnosticsJson(""));

    const fs::path renderGraphDiagnosticsJsonPath =
        fs::temp_directory_path() / ("RVX_SceneRendererToolRenderGraphDiagnostics_" + suffix + ".json");
    const std::string renderGraphDiagnosticsJsonPathString = renderGraphDiagnosticsJsonPath.string();
    ASSERT_TRUE(renderer.SaveToolRenderGraphDiagnosticsJson(renderGraphDiagnosticsJsonPathString.c_str()));
    const std::string savedRenderGraphDiagnosticsJson = ReadTextFile(renderGraphDiagnosticsJsonPath);
    EXPECT_EQ(savedRenderGraphDiagnosticsJson, renderGraphDiagnosticsJson);
    fs::remove(renderGraphDiagnosticsJsonPath, removeError);

    const std::string manifestJson = renderer.ExportToolDiagnosticsManifestJson();
    EXPECT_NE(manifestJson.find(
                  "\"schemaVersion\": " + std::to_string(
                      RVX_SCENE_RENDERER_TOOL_DIAGNOSTICS_SCHEMA_VERSION)),
              std::string::npos);
    EXPECT_NE(manifestJson.find("\"id\": \"manifestJson\""), std::string::npos);
    EXPECT_NE(manifestJson.find("\"kind\": \"ToolDiagnosticsManifestJson\""), std::string::npos);
    EXPECT_NE(manifestJson.find("\"contentType\": \"application/json\""), std::string::npos);
    EXPECT_NE(manifestJson.find("\"contentHash\": \"\""), std::string::npos);
    EXPECT_NE(manifestJson.find("\"relativePath\": \"\""), std::string::npos);
    EXPECT_NE(manifestJson.find("\"frameDiagnosticsAvailable\": true"), std::string::npos);
    EXPECT_NE(manifestJson.find("\"renderGraphDiagnosticsAvailable\": true"), std::string::npos);
    EXPECT_NE(manifestJson.find("\"rhiCapabilityReportAvailable\": false"), std::string::npos);
    EXPECT_NE(manifestJson.find("\"rhiCapabilities\": null"), std::string::npos);
    EXPECT_NE(manifestJson.find(
                  "\"renderGraph\": {\n    \"schemaVersion\": " +
                  std::to_string(RVX_RENDER_GRAPH_DIAGNOSTICS_SCHEMA_VERSION)),
              std::string::npos);
    EXPECT_NE(manifestJson.find("\"schemaId\": \"RVX.RenderGraph.Diagnostics\""), std::string::npos);
    EXPECT_NE(manifestJson.find("\"resourceCount\": 1"), std::string::npos);
    EXPECT_NE(manifestJson.find("\"artifacts\": null"), std::string::npos);
    EXPECT_FALSE(renderer.SaveToolDiagnosticsManifestJson(nullptr));
    EXPECT_FALSE(renderer.SaveToolDiagnosticsManifestJson(""));

    const fs::path manifestPath =
        fs::temp_directory_path() / ("RVX_SceneRendererToolDiagnosticsManifest_" + suffix + ".json");
    const std::string manifestPathString = manifestPath.string();
    ASSERT_TRUE(renderer.SaveToolDiagnosticsManifestJson(manifestPathString.c_str()));
    EXPECT_EQ(ReadTextFile(manifestPath), manifestJson);
    fs::remove(manifestPath, removeError);

    const std::string emptyArtifactSummaryJson = renderer.ExportToolDiagnosticsArtifactSummaryJson();
    EXPECT_NE(emptyArtifactSummaryJson.find("\"schemaVersion\": 24"), std::string::npos);
    EXPECT_NE(emptyArtifactSummaryJson.find("\"id\": \"artifactSummaryJson\""), std::string::npos);
    EXPECT_NE(emptyArtifactSummaryJson.find("\"kind\": \"ToolDiagnosticsArtifactSummaryJson\""),
              std::string::npos);
    EXPECT_NE(emptyArtifactSummaryJson.find("\"contentType\": \"application/json\""), std::string::npos);
    EXPECT_NE(emptyArtifactSummaryJson.find("\"contentHash\": \"\""), std::string::npos);
    EXPECT_NE(emptyArtifactSummaryJson.find("\"relativePath\": \"\""), std::string::npos);
    EXPECT_NE(emptyArtifactSummaryJson.find(
                  "\"toolDiagnosticsSchemaVersion\": " + std::to_string(
                      RVX_SCENE_RENDERER_TOOL_DIAGNOSTICS_SCHEMA_VERSION)),
              std::string::npos);
    EXPECT_NE(emptyArtifactSummaryJson.find("\"artifactResultAvailable\": false"), std::string::npos);
    EXPECT_NE(emptyArtifactSummaryJson.find("\"metadataAvailable\": false"), std::string::npos);
    EXPECT_NE(emptyArtifactSummaryJson.find("\"captureId\": \"\""), std::string::npos);
    EXPECT_NE(emptyArtifactSummaryJson.find("\"baseName\": \"\""), std::string::npos);
    EXPECT_NE(emptyArtifactSummaryJson.find("\"frameIndex\": 0"), std::string::npos);
    EXPECT_NE(emptyArtifactSummaryJson.find(
                  "\"renderGraphDiagnosticsSchemaVersion\": " +
                  std::to_string(RVX_RENDER_GRAPH_DIAGNOSTICS_SCHEMA_VERSION)),
              std::string::npos);
    EXPECT_NE(emptyArtifactSummaryJson.find("\"renderGraphDiagnosticsSchemaId\": \"RVX.RenderGraph.Diagnostics\""),
              std::string::npos);
    EXPECT_NE(emptyArtifactSummaryJson.find("\"artifactSummarySchemaVersion\": 24"), std::string::npos);
    EXPECT_NE(emptyArtifactSummaryJson.find("\"artifactValidationSchemaVersion\": 25"), std::string::npos);
    EXPECT_NE(emptyArtifactSummaryJson.find("\"artifactCount\": 0"), std::string::npos);
    EXPECT_NE(emptyArtifactSummaryJson.find("\"allArtifactsSaved\": false"), std::string::npos);
    EXPECT_NE(emptyArtifactSummaryJson.find("\"totalArtifactBytes\": 0"), std::string::npos);
    EXPECT_NE(emptyArtifactSummaryJson.find("\"primaryArtifactBundleHash\": \"\""), std::string::npos);
    EXPECT_NE(emptyArtifactSummaryJson.find("\"artifacts\": []"), std::string::npos);
    EXPECT_NE(emptyArtifactSummaryJson.find("\"validationReport\": {"), std::string::npos);
    EXPECT_NE(emptyArtifactSummaryJson.find("\"id\": \"artifactValidationJson\""), std::string::npos);
    EXPECT_NE(emptyArtifactSummaryJson.find("\"schemaVersion\": 25"), std::string::npos);
    EXPECT_NE(emptyArtifactSummaryJson.find("\"resultAvailable\": false"), std::string::npos);
    EXPECT_NE(emptyArtifactSummaryJson.find("\"allPrimaryArtifactsValid\": false"), std::string::npos);
    EXPECT_NE(emptyArtifactSummaryJson.find("\"bundleHashMatches\": false"), std::string::npos);
    EXPECT_NE(emptyArtifactSummaryJson.find("\"verdictCode\": \"Unavailable\""), std::string::npos);
    EXPECT_NE(emptyArtifactSummaryJson.find("\"primaryFailureCode\": \"Unavailable\""), std::string::npos);
    EXPECT_NE(emptyArtifactSummaryJson.find("\"primaryFailureEntryIndex\": null"), std::string::npos);
    EXPECT_NE(emptyArtifactSummaryJson.find("\"primaryFailureEntryCount\": 0"), std::string::npos);
    EXPECT_NE(emptyArtifactSummaryJson.find("\"entryCount\": 0"), std::string::npos);
    EXPECT_NE(emptyArtifactSummaryJson.find("\"entryCountMatchesCheckedPrimaryArtifactCount\": false"),
              std::string::npos);
    EXPECT_NE(emptyArtifactSummaryJson.find("\"entryCoverageCode\": \"Unavailable\""), std::string::npos);
    EXPECT_NE(emptyArtifactSummaryJson.find("\"entryCoverageMessage\": \"validation result is unavailable\""),
              std::string::npos);
    EXPECT_NE(emptyArtifactSummaryJson.find("\"primaryFailureArtifactId\": \"\""), std::string::npos);
    EXPECT_NE(emptyArtifactSummaryJson.find("\"primaryFailureArtifactRelativePath\": \"\""),
              std::string::npos);
    EXPECT_NE(emptyArtifactSummaryJson.find("\"primaryFailureArtifactKind\": \"\""), std::string::npos);
    EXPECT_NE(emptyArtifactSummaryJson.find("\"primaryFailureArtifactContentType\": \"\""),
              std::string::npos);
    EXPECT_NE(emptyArtifactSummaryJson.find("\"primaryFailureArtifactSchemaId\": \"\""),
              std::string::npos);
    EXPECT_NE(emptyArtifactSummaryJson.find("\"primaryFailureArtifactSchemaVersion\": 0"),
              std::string::npos);
    EXPECT_NE(emptyArtifactSummaryJson.find("\"primaryFailureMessage\": \"validation result is unavailable\""),
              std::string::npos);
    EXPECT_NE(emptyArtifactSummaryJson.find("\"checkedPrimaryArtifactCount\": 0"), std::string::npos);
    EXPECT_NE(emptyArtifactSummaryJson.find("\"validPrimaryArtifactCount\": 0"), std::string::npos);
    EXPECT_NE(emptyArtifactSummaryJson.find("\"failedPrimaryArtifactCount\": 0"), std::string::npos);
    EXPECT_NE(emptyArtifactSummaryJson.find("\"diagnosticCodeCounts\": []"), std::string::npos);
    EXPECT_NE(emptyArtifactSummaryJson.find("\"actualTotalPrimaryArtifactBytes\": 0"), std::string::npos);
    EXPECT_NE(emptyArtifactSummaryJson.find("\"expectedPrimaryArtifactBundleHash\": \"\""), std::string::npos);
    EXPECT_NE(emptyArtifactSummaryJson.find("\"actualPrimaryArtifactBundleHash\": \"\""), std::string::npos);
    EXPECT_FALSE(renderer.SaveToolDiagnosticsArtifactSummaryJson(nullptr));
    EXPECT_FALSE(renderer.SaveToolDiagnosticsArtifactSummaryJson(""));

    const fs::path emptyArtifactSummaryPath =
        fs::temp_directory_path() / ("RVX_SceneRendererToolArtifactSummary_" + suffix + ".json");
    const std::string emptyArtifactSummaryPathString = emptyArtifactSummaryPath.string();
    ASSERT_TRUE(renderer.SaveToolDiagnosticsArtifactSummaryJson(emptyArtifactSummaryPathString.c_str()));
    EXPECT_EQ(ReadTextFile(emptyArtifactSummaryPath), emptyArtifactSummaryJson);
    fs::remove(emptyArtifactSummaryPath, removeError);

    SceneRendererToolDiagnosticsArtifactResult invalidArtifactResult =
        renderer.SaveToolDiagnosticsArtifacts(nullptr, "Frame");
    EXPECT_TRUE(invalidArtifactResult.requested);
    EXPECT_FALSE(invalidArtifactResult.directoryReady);

    invalidArtifactResult = renderer.SaveToolDiagnosticsArtifacts(fs::temp_directory_path().string().c_str(), "");
    EXPECT_TRUE(invalidArtifactResult.requested);
    EXPECT_FALSE(invalidArtifactResult.directoryReady);

    const fs::path artifactDirectory =
        fs::temp_directory_path() / ("RVX_SceneRendererToolArtifacts_" + suffix);
    const std::string artifactDirectoryString = artifactDirectory.string();
    const SceneRendererToolDiagnosticsArtifactResult artifactResult =
        renderer.SaveToolDiagnosticsArtifacts(artifactDirectoryString.c_str(), "Frame001");
    EXPECT_TRUE(artifactResult.requested);
    EXPECT_TRUE(artifactResult.directoryReady);
    EXPECT_TRUE(artifactResult.captureMetadataAvailable);
    EXPECT_EQ(artifactResult.captureId.size(), 16u);
    EXPECT_EQ(artifactResult.captureBaseName, "Frame001");
    EXPECT_EQ(artifactResult.outputDirectory, artifactDirectoryString);
    EXPECT_EQ(artifactResult.toolDiagnosticsSchemaVersion, RVX_SCENE_RENDERER_TOOL_DIAGNOSTICS_SCHEMA_VERSION);
    EXPECT_EQ(artifactResult.frameDiagnosticsSchemaVersion, RVX_SCENE_RENDERER_FRAME_DIAGNOSTICS_SCHEMA_VERSION);
    EXPECT_EQ(artifactResult.renderGraphDiagnosticsSchemaVersion, RVX_RENDER_GRAPH_DIAGNOSTICS_SCHEMA_VERSION);
    EXPECT_EQ(artifactResult.artifactSummarySchemaVersion, RVX_SCENE_RENDERER_TOOL_ARTIFACT_SUMMARY_SCHEMA_VERSION);
    EXPECT_EQ(artifactResult.artifactValidationSchemaVersion,
              RVX_SCENE_RENDERER_TOOL_ARTIFACT_VALIDATION_SCHEMA_VERSION);
    EXPECT_EQ(artifactResult.renderGraphDiagnosticsSchemaId, RVX_RENDER_GRAPH_DIAGNOSTICS_SCHEMA_ID);
    EXPECT_EQ(artifactResult.frameIndex, toolSnapshot.frame.frameCount);
    EXPECT_EQ(artifactResult.renderGraphPassCount, toolSnapshot.renderGraph.passes.size());
    EXPECT_EQ(artifactResult.renderGraphResourceCount, toolSnapshot.renderGraph.resources.size());
    EXPECT_TRUE(artifactResult.toolDiagnosticsTextSaved);
    EXPECT_TRUE(artifactResult.renderGraphGraphvizSaved);
    EXPECT_TRUE(artifactResult.renderGraphDiagnosticsTextSaved);
    EXPECT_TRUE(artifactResult.renderGraphDiagnosticsJsonSaved);
    EXPECT_TRUE(artifactResult.manifestJsonSaved);
    EXPECT_TRUE(artifactResult.artifactSummaryJsonSaved);
    EXPECT_TRUE(artifactResult.artifactValidationJsonSaved);
    EXPECT_TRUE(artifactResult.allPrimaryArtifactsSaved);
    EXPECT_TRUE(artifactResult.artifactValidationResultAvailable);
    EXPECT_TRUE(artifactResult.artifactValidationAllPrimaryArtifactsValid);
    EXPECT_TRUE(artifactResult.artifactValidationBundleHashMatches);
    EXPECT_EQ(artifactResult.artifactValidationVerdictCode, "Valid");
    EXPECT_EQ(artifactResult.artifactValidationPrimaryFailureCode, "None");
    EXPECT_EQ(artifactResult.artifactValidationPrimaryFailureEntryIndex, RVX_INVALID_INDEX);
    EXPECT_EQ(artifactResult.artifactValidationPrimaryFailureEntryCount, 0u);
    EXPECT_EQ(artifactResult.artifactValidationEntryCount, 5u);
    EXPECT_TRUE(artifactResult.artifactValidationEntryCountMatchesCheckedPrimaryArtifactCount);
    EXPECT_EQ(artifactResult.artifactValidationEntryCoverageCode, "Complete");
    EXPECT_EQ(artifactResult.artifactValidationEntryCoverageMessage,
              "validation entries cover all checked primary artifacts");
    EXPECT_TRUE(artifactResult.artifactValidationPrimaryFailureArtifactId.empty());
    EXPECT_TRUE(artifactResult.artifactValidationPrimaryFailureArtifactRelativePath.empty());
    EXPECT_TRUE(artifactResult.artifactValidationPrimaryFailureArtifactKind.empty());
    EXPECT_TRUE(artifactResult.artifactValidationPrimaryFailureArtifactContentType.empty());
    EXPECT_TRUE(artifactResult.artifactValidationPrimaryFailureArtifactSchemaId.empty());
    EXPECT_EQ(artifactResult.artifactValidationPrimaryFailureArtifactSchemaVersion, 0u);
    EXPECT_TRUE(artifactResult.artifactValidationPrimaryFailureMessage.empty());
    EXPECT_EQ(artifactResult.primaryArtifactCount, 5u);
    EXPECT_EQ(artifactResult.savedPrimaryArtifactCount, 5u);
    EXPECT_EQ(artifactResult.artifactValidationCheckedPrimaryArtifactCount, 5u);
    EXPECT_EQ(artifactResult.artifactValidationValidPrimaryArtifactCount, 5u);
    EXPECT_EQ(artifactResult.artifactValidationFailedPrimaryArtifactCount, 0u);
    ASSERT_EQ(artifactResult.artifactValidationDiagnosticCodeCounts.size(), 1u);
    EXPECT_EQ(artifactResult.artifactValidationDiagnosticCodeCounts[0].code, "None");
    EXPECT_EQ(artifactResult.artifactValidationDiagnosticCodeCounts[0].count, 5u);
    EXPECT_TRUE(artifactResult.toolDiagnosticsTextExists);
    EXPECT_TRUE(artifactResult.renderGraphGraphvizExists);
    EXPECT_TRUE(artifactResult.renderGraphDiagnosticsTextExists);
    EXPECT_TRUE(artifactResult.renderGraphDiagnosticsJsonExists);
    EXPECT_TRUE(artifactResult.manifestJsonExists);
    EXPECT_TRUE(artifactResult.artifactValidationJsonExists);
    EXPECT_GT(artifactResult.toolDiagnosticsTextBytes, 0u);
    EXPECT_GT(artifactResult.renderGraphGraphvizBytes, 0u);
    EXPECT_GT(artifactResult.renderGraphDiagnosticsTextBytes, 0u);
    EXPECT_GT(artifactResult.renderGraphDiagnosticsJsonBytes, 0u);
    EXPECT_GT(artifactResult.manifestJsonBytes, 0u);
    EXPECT_GT(artifactResult.artifactValidationJsonBytes, 0u);
    EXPECT_EQ(artifactResult.toolDiagnosticsTextContentHash.size(), 16u);
    EXPECT_EQ(artifactResult.renderGraphGraphvizContentHash.size(), 16u);
    EXPECT_EQ(artifactResult.renderGraphDiagnosticsTextContentHash.size(), 16u);
    EXPECT_EQ(artifactResult.renderGraphDiagnosticsJsonContentHash.size(), 16u);
    EXPECT_EQ(artifactResult.manifestJsonContentHash.size(), 16u);
    EXPECT_EQ(artifactResult.artifactValidationJsonContentHash.size(), 16u);
    EXPECT_EQ(artifactResult.primaryArtifactBundleHash.size(), 16u);
    EXPECT_EQ(artifactResult.artifactValidationExpectedPrimaryArtifactBundleHash,
              artifactResult.primaryArtifactBundleHash);
    EXPECT_EQ(artifactResult.artifactValidationActualPrimaryArtifactBundleHash,
              artifactResult.primaryArtifactBundleHash);
    EXPECT_EQ(artifactResult.totalPrimaryArtifactBytes,
              artifactResult.toolDiagnosticsTextBytes +
                  artifactResult.renderGraphGraphvizBytes +
                  artifactResult.renderGraphDiagnosticsTextBytes +
                  artifactResult.renderGraphDiagnosticsJsonBytes +
                  artifactResult.manifestJsonBytes);
    EXPECT_EQ(artifactResult.artifactValidationActualTotalPrimaryArtifactBytes,
              artifactResult.totalPrimaryArtifactBytes);
    EXPECT_NE(artifactResult.toolDiagnosticsTextPath.find("Frame001.scene-renderer.txt"), std::string::npos);
    EXPECT_NE(artifactResult.renderGraphGraphvizPath.find("Frame001.rendergraph.dot"), std::string::npos);
    EXPECT_NE(artifactResult.renderGraphDiagnosticsTextPath.find("Frame001.rendergraph.txt"), std::string::npos);
    EXPECT_NE(artifactResult.renderGraphDiagnosticsJsonPath.find("Frame001.rendergraph.json"), std::string::npos);
    EXPECT_NE(artifactResult.manifestJsonPath.find("Frame001.diagnostics-manifest.json"), std::string::npos);
    EXPECT_NE(artifactResult.artifactSummaryJsonPath.find("Frame001.diagnostics-artifacts.json"), std::string::npos);
    EXPECT_NE(artifactResult.artifactValidationJsonPath.find("Frame001.diagnostics-validation.json"),
              std::string::npos);
    EXPECT_EQ(artifactResult.toolDiagnosticsTextRelativePath, "Frame001.scene-renderer.txt");
    EXPECT_EQ(artifactResult.renderGraphGraphvizRelativePath, "Frame001.rendergraph.dot");
    EXPECT_EQ(artifactResult.renderGraphDiagnosticsTextRelativePath, "Frame001.rendergraph.txt");
    EXPECT_EQ(artifactResult.renderGraphDiagnosticsJsonRelativePath, "Frame001.rendergraph.json");
    EXPECT_EQ(artifactResult.manifestJsonRelativePath, "Frame001.diagnostics-manifest.json");
    EXPECT_EQ(artifactResult.artifactSummaryJsonRelativePath, "Frame001.diagnostics-artifacts.json");
    EXPECT_EQ(artifactResult.artifactValidationJsonRelativePath, "Frame001.diagnostics-validation.json");
    EXPECT_EQ(ReadTextFile(artifactResult.toolDiagnosticsTextPath), diagnosticsText);
    EXPECT_EQ(ReadTextFile(artifactResult.renderGraphGraphvizPath), graphviz);
    EXPECT_EQ(ReadTextFile(artifactResult.renderGraphDiagnosticsTextPath), renderGraphDiagnosticsText);
    EXPECT_EQ(ReadTextFile(artifactResult.renderGraphDiagnosticsJsonPath), renderGraphDiagnosticsJson);
    const std::string artifactSummaryJson = ReadTextFile(artifactResult.artifactSummaryJsonPath);
    EXPECT_NE(artifactSummaryJson.find("\"schemaVersion\": 24"), std::string::npos);
    EXPECT_NE(artifactSummaryJson.find("\"id\": \"artifactSummaryJson\""), std::string::npos);
    EXPECT_NE(artifactSummaryJson.find("\"kind\": \"ToolDiagnosticsArtifactSummaryJson\""), std::string::npos);
    EXPECT_NE(artifactSummaryJson.find("\"contentType\": \"application/json\""), std::string::npos);
    EXPECT_NE(artifactSummaryJson.find("\"contentHash\": \"\""), std::string::npos);
    EXPECT_NE(artifactSummaryJson.find("\"relativePath\": \"\""), std::string::npos);
    EXPECT_NE(artifactSummaryJson.find(
                  "\"toolDiagnosticsSchemaVersion\": " + std::to_string(
                      RVX_SCENE_RENDERER_TOOL_DIAGNOSTICS_SCHEMA_VERSION)),
              std::string::npos);
    EXPECT_NE(artifactSummaryJson.find("\"artifactResultAvailable\": true"), std::string::npos);
    EXPECT_NE(artifactSummaryJson.find("\"capture\": {"), std::string::npos);
    EXPECT_NE(artifactSummaryJson.find("\"metadataAvailable\": true"), std::string::npos);
    EXPECT_NE(artifactSummaryJson.find("\"captureId\": \"" + artifactResult.captureId + "\""),
              std::string::npos);
    EXPECT_NE(artifactSummaryJson.find("\"baseName\": \"Frame001\""), std::string::npos);
    EXPECT_NE(artifactSummaryJson.find("\"outputDirectory\": "), std::string::npos);
    EXPECT_NE(artifactSummaryJson.find("\"frameIndex\": " + std::to_string(toolSnapshot.frame.frameCount)),
              std::string::npos);
    EXPECT_NE(artifactSummaryJson.find(
                  "\"renderGraphDiagnosticsSchemaVersion\": " +
                  std::to_string(RVX_RENDER_GRAPH_DIAGNOSTICS_SCHEMA_VERSION)),
              std::string::npos);
    EXPECT_NE(artifactSummaryJson.find("\"renderGraphDiagnosticsSchemaId\": \"RVX.RenderGraph.Diagnostics\""),
              std::string::npos);
    EXPECT_NE(artifactSummaryJson.find("\"artifactSummarySchemaVersion\": 24"), std::string::npos);
    EXPECT_NE(artifactSummaryJson.find("\"artifactValidationSchemaVersion\": 25"), std::string::npos);
    EXPECT_NE(artifactSummaryJson.find("\"renderGraphPassCount\": " +
                                       std::to_string(toolSnapshot.renderGraph.passes.size())),
              std::string::npos);
    EXPECT_NE(artifactSummaryJson.find("\"renderGraphResourceCount\": " +
                                       std::to_string(toolSnapshot.renderGraph.resources.size())),
              std::string::npos);
    EXPECT_NE(artifactSummaryJson.find("\"artifactCount\": 5"), std::string::npos);
    EXPECT_NE(artifactSummaryJson.find("\"savedCount\": 5"), std::string::npos);
    EXPECT_NE(artifactSummaryJson.find("\"allArtifactsSaved\": true"), std::string::npos);
    EXPECT_NE(artifactSummaryJson.find("\"totalArtifactBytes\": "), std::string::npos);
    EXPECT_NE(artifactSummaryJson.find("\"primaryArtifactBundleHash\": \"" +
                                       artifactResult.primaryArtifactBundleHash + "\""),
              std::string::npos);
    EXPECT_NE(artifactSummaryJson.find("\"id\": \"renderGraphDiagnosticsJson\""), std::string::npos);
    EXPECT_NE(artifactSummaryJson.find("\"kind\": \"RenderGraphDiagnosticsJson\""), std::string::npos);
    EXPECT_NE(artifactSummaryJson.find("\"contentType\": \"application/json\""), std::string::npos);
    EXPECT_NE(artifactSummaryJson.find("\"schemaId\": \"RVX.RenderGraph.Diagnostics\""), std::string::npos);
    EXPECT_NE(artifactSummaryJson.find(
                  "\"schemaVersion\": " + std::to_string(RVX_RENDER_GRAPH_DIAGNOSTICS_SCHEMA_VERSION)),
              std::string::npos);
    EXPECT_NE(artifactSummaryJson.find("\"exists\": true"), std::string::npos);
    EXPECT_NE(artifactSummaryJson.find("\"byteSize\": "), std::string::npos);
    EXPECT_NE(artifactSummaryJson.find("\"contentHash\": \"" +
                                       artifactResult.renderGraphDiagnosticsJsonContentHash + "\""),
              std::string::npos);
    EXPECT_NE(artifactSummaryJson.find("\"contentHash\": \"" +
                                       artifactResult.manifestJsonContentHash + "\""),
              std::string::npos);
    EXPECT_NE(artifactSummaryJson.find("\"relativePath\": \"Frame001.rendergraph.json\""), std::string::npos);
    EXPECT_NE(artifactSummaryJson.find("\"relativePath\": \"Frame001.diagnostics-manifest.json\""),
              std::string::npos);
    EXPECT_NE(artifactSummaryJson.find("\"validationReport\": {"), std::string::npos);
    EXPECT_NE(artifactSummaryJson.find("\"id\": \"artifactValidationJson\""), std::string::npos);
    EXPECT_NE(artifactSummaryJson.find("\"schemaVersion\": 25"), std::string::npos);
    EXPECT_NE(artifactSummaryJson.find("\"resultAvailable\": true"), std::string::npos);
    EXPECT_NE(artifactSummaryJson.find("\"allPrimaryArtifactsValid\": true"), std::string::npos);
    EXPECT_NE(artifactSummaryJson.find("\"bundleHashMatches\": true"), std::string::npos);
    EXPECT_NE(artifactSummaryJson.find("\"verdictCode\": \"Valid\""), std::string::npos);
    EXPECT_NE(artifactSummaryJson.find("\"primaryFailureCode\": \"None\""), std::string::npos);
    EXPECT_NE(artifactSummaryJson.find("\"primaryFailureEntryIndex\": null"), std::string::npos);
    EXPECT_NE(artifactSummaryJson.find("\"primaryFailureEntryCount\": 0"), std::string::npos);
    EXPECT_NE(artifactSummaryJson.find("\"entryCount\": 5"), std::string::npos);
    EXPECT_NE(artifactSummaryJson.find("\"entryCountMatchesCheckedPrimaryArtifactCount\": true"),
              std::string::npos);
    EXPECT_NE(artifactSummaryJson.find("\"entryCoverageCode\": \"Complete\""), std::string::npos);
    EXPECT_NE(artifactSummaryJson.find("\"entryCoverageMessage\": \"validation entries cover all checked primary artifacts\""),
              std::string::npos);
    EXPECT_NE(artifactSummaryJson.find("\"primaryFailureArtifactId\": \"\""), std::string::npos);
    EXPECT_NE(artifactSummaryJson.find("\"primaryFailureArtifactRelativePath\": \"\""),
              std::string::npos);
    EXPECT_NE(artifactSummaryJson.find("\"primaryFailureArtifactKind\": \"\""), std::string::npos);
    EXPECT_NE(artifactSummaryJson.find("\"primaryFailureArtifactContentType\": \"\""),
              std::string::npos);
    EXPECT_NE(artifactSummaryJson.find("\"primaryFailureArtifactSchemaId\": \"\""), std::string::npos);
    EXPECT_NE(artifactSummaryJson.find("\"primaryFailureArtifactSchemaVersion\": 0"),
              std::string::npos);
    EXPECT_NE(artifactSummaryJson.find("\"primaryFailureMessage\": \"\""), std::string::npos);
    EXPECT_NE(artifactSummaryJson.find("\"checkedPrimaryArtifactCount\": 5"), std::string::npos);
    EXPECT_NE(artifactSummaryJson.find("\"validPrimaryArtifactCount\": 5"), std::string::npos);
    EXPECT_NE(artifactSummaryJson.find("\"failedPrimaryArtifactCount\": 0"), std::string::npos);
    EXPECT_NE(artifactSummaryJson.find("\"diagnosticCodeCounts\": ["), std::string::npos);
    EXPECT_NE(artifactSummaryJson.find("\"code\": \"None\""), std::string::npos);
    EXPECT_NE(artifactSummaryJson.find("\"count\": 5"), std::string::npos);
    EXPECT_NE(artifactSummaryJson.find("\"actualTotalPrimaryArtifactBytes\": " +
                                       std::to_string(artifactResult.totalPrimaryArtifactBytes)),
              std::string::npos);
    EXPECT_NE(artifactSummaryJson.find("\"expectedPrimaryArtifactBundleHash\": \"" +
                                       artifactResult.primaryArtifactBundleHash + "\""),
              std::string::npos);
    EXPECT_NE(artifactSummaryJson.find("\"actualPrimaryArtifactBundleHash\": \"" +
                                       artifactResult.primaryArtifactBundleHash + "\""),
              std::string::npos);
    EXPECT_NE(artifactSummaryJson.find("\"relativePath\": \"Frame001.diagnostics-validation.json\""),
              std::string::npos);
    EXPECT_NE(artifactSummaryJson.find("\"contentHash\": \"" +
                                       artifactResult.artifactValidationJsonContentHash + "\""),
              std::string::npos);
    EXPECT_NE(artifactSummaryJson.find("Frame001.rendergraph.json"), std::string::npos);
    EXPECT_NE(artifactSummaryJson.find("Frame001.diagnostics-manifest.json"), std::string::npos);
    const std::string artifactManifestJson = ReadTextFile(artifactResult.manifestJsonPath);
    EXPECT_NE(artifactManifestJson.find(
                  "\"schemaVersion\": " + std::to_string(
                      RVX_SCENE_RENDERER_TOOL_DIAGNOSTICS_SCHEMA_VERSION)),
              std::string::npos);
    EXPECT_NE(artifactManifestJson.find("\"id\": \"manifestJson\""), std::string::npos);
    EXPECT_NE(artifactManifestJson.find("\"kind\": \"ToolDiagnosticsManifestJson\""), std::string::npos);
    EXPECT_NE(artifactManifestJson.find("\"contentType\": \"application/json\""), std::string::npos);
    EXPECT_NE(artifactManifestJson.find("\"artifacts\": {"), std::string::npos);
    EXPECT_NE(artifactManifestJson.find("\"capture\": {"), std::string::npos);
    EXPECT_NE(artifactManifestJson.find("\"captureId\": \"" + artifactResult.captureId + "\""),
              std::string::npos);
    EXPECT_NE(artifactManifestJson.find("\"baseName\": \"Frame001\""), std::string::npos);
    EXPECT_NE(artifactManifestJson.find("\"frameIndex\": " + std::to_string(toolSnapshot.frame.frameCount)),
              std::string::npos);
    EXPECT_NE(artifactManifestJson.find(
                  "\"renderGraphDiagnosticsSchemaVersion\": " +
                  std::to_string(RVX_RENDER_GRAPH_DIAGNOSTICS_SCHEMA_VERSION)),
              std::string::npos);
    EXPECT_NE(artifactManifestJson.find("\"renderGraphDiagnosticsSchemaId\": \"RVX.RenderGraph.Diagnostics\""),
              std::string::npos);
    EXPECT_NE(artifactManifestJson.find("\"renderGraphPassCount\": " +
                                        std::to_string(toolSnapshot.renderGraph.passes.size())),
              std::string::npos);
    EXPECT_NE(artifactManifestJson.find("\"renderGraphResourceCount\": " +
                                        std::to_string(toolSnapshot.renderGraph.resources.size())),
              std::string::npos);
    EXPECT_NE(artifactManifestJson.find("\"artifactSummarySchemaVersion\": 24"), std::string::npos);
    EXPECT_NE(artifactManifestJson.find("\"artifactValidationSchemaVersion\": 25"), std::string::npos);
    EXPECT_NE(artifactManifestJson.find("\"primaryArtifactCount\": 5"), std::string::npos);
    EXPECT_NE(artifactManifestJson.find("\"savedPrimaryArtifactCount\": 5"), std::string::npos);
    EXPECT_NE(artifactManifestJson.find("\"allPrimaryArtifactsSaved\": true"), std::string::npos);
    EXPECT_NE(artifactManifestJson.find("\"totalPrimaryArtifactBytes\": "), std::string::npos);
    EXPECT_NE(artifactManifestJson.find("\"sceneRendererText\": {"), std::string::npos);
    EXPECT_NE(artifactManifestJson.find("\"kind\": \"SceneRendererText\""), std::string::npos);
    EXPECT_NE(artifactManifestJson.find("\"renderGraphGraphviz\": {"), std::string::npos);
    EXPECT_NE(artifactManifestJson.find("\"kind\": \"RenderGraphGraphviz\""), std::string::npos);
    EXPECT_NE(artifactManifestJson.find("\"contentType\": \"text/vnd.graphviz\""), std::string::npos);
    EXPECT_NE(artifactManifestJson.find("\"renderGraphDiagnosticsJson\": {"), std::string::npos);
    EXPECT_NE(artifactManifestJson.find("\"kind\": \"RenderGraphDiagnosticsJson\""), std::string::npos);
    EXPECT_NE(artifactManifestJson.find("\"schemaId\": \"RVX.RenderGraph.Diagnostics\""), std::string::npos);
    EXPECT_NE(artifactManifestJson.find(
                  "\"schemaVersion\": " + std::to_string(RVX_RENDER_GRAPH_DIAGNOSTICS_SCHEMA_VERSION)),
              std::string::npos);
    EXPECT_NE(artifactManifestJson.find("\"manifestJson\": {"), std::string::npos);
    EXPECT_NE(artifactManifestJson.find("\"kind\": \"ToolDiagnosticsManifestJson\""), std::string::npos);
    EXPECT_NE(artifactManifestJson.find("\"kind\": \"ToolDiagnosticsArtifactSummaryJson\""), std::string::npos);
    EXPECT_NE(artifactManifestJson.find("\"schemaVersion\": 24"), std::string::npos);
    EXPECT_NE(artifactManifestJson.find("\"relativePath\": \"Frame001.scene-renderer.txt\""), std::string::npos);
    EXPECT_NE(artifactManifestJson.find("\"relativePath\": \"Frame001.rendergraph.dot\""), std::string::npos);
    EXPECT_NE(artifactManifestJson.find("\"relativePath\": \"Frame001.rendergraph.txt\""), std::string::npos);
    EXPECT_NE(artifactManifestJson.find("\"relativePath\": \"Frame001.rendergraph.json\""), std::string::npos);
    EXPECT_NE(artifactManifestJson.find("\"relativePath\": \"Frame001.diagnostics-manifest.json\""),
              std::string::npos);
    EXPECT_NE(artifactManifestJson.find("\"relativePath\": \"Frame001.diagnostics-artifacts.json\""),
              std::string::npos);
    EXPECT_NE(artifactManifestJson.find("Frame001.scene-renderer.txt"), std::string::npos);
    EXPECT_NE(artifactManifestJson.find("Frame001.rendergraph.dot"), std::string::npos);
    EXPECT_NE(artifactManifestJson.find("Frame001.rendergraph.txt"), std::string::npos);
    EXPECT_NE(artifactManifestJson.find("Frame001.rendergraph.json"), std::string::npos);
    EXPECT_NE(artifactManifestJson.find("\"artifactSummaryJson\": {"), std::string::npos);
    EXPECT_NE(artifactManifestJson.find("Frame001.diagnostics-artifacts.json"), std::string::npos);
    EXPECT_NE(artifactManifestJson.find("\"artifactValidationJson\": {"), std::string::npos);
    EXPECT_NE(artifactManifestJson.find("\"kind\": \"ToolDiagnosticsArtifactValidationJson\""), std::string::npos);
    EXPECT_NE(artifactManifestJson.find("\"contentType\": \"application/json\""), std::string::npos);
    EXPECT_NE(artifactManifestJson.find("\"schemaVersion\": 25"), std::string::npos);
    EXPECT_NE(artifactManifestJson.find("\"relativePath\": \"Frame001.diagnostics-validation.json\""),
              std::string::npos);
    EXPECT_NE(artifactManifestJson.find("Frame001.diagnostics-validation.json"), std::string::npos);
    EXPECT_EQ(artifactManifestJson.find("\"validationReport\": {"), std::string::npos);
    EXPECT_EQ(artifactManifestJson.find("\"diagnosticCodeCounts\": ["), std::string::npos);

    const SceneRendererToolDiagnosticsArtifactValidationResult validation =
        SceneRenderer::ValidateToolDiagnosticsArtifacts(artifactResult);
    EXPECT_EQ(validation.schemaVersion, RVX_SCENE_RENDERER_TOOL_ARTIFACT_VALIDATION_SCHEMA_VERSION);
    EXPECT_TRUE(validation.artifactResultAvailable);
    EXPECT_TRUE(validation.allPrimaryArtifactsValid);
    EXPECT_TRUE(validation.bundleHashMatches);
    EXPECT_EQ(validation.verdictCode, "Valid");
    EXPECT_EQ(validation.primaryFailureCode, "None");
    EXPECT_EQ(validation.primaryFailureEntryIndex, RVX_INVALID_INDEX);
    EXPECT_EQ(validation.primaryFailureEntryCount, 0u);
    EXPECT_EQ(validation.entryCount, 5u);
    EXPECT_TRUE(validation.entryCountMatchesCheckedPrimaryArtifactCount);
    EXPECT_EQ(validation.entryCoverageCode, "Complete");
    EXPECT_EQ(validation.entryCoverageMessage, "validation entries cover all checked primary artifacts");
    EXPECT_TRUE(validation.primaryFailureArtifactId.empty());
    EXPECT_TRUE(validation.primaryFailureArtifactRelativePath.empty());
    EXPECT_TRUE(validation.primaryFailureArtifactKind.empty());
    EXPECT_TRUE(validation.primaryFailureArtifactContentType.empty());
    EXPECT_TRUE(validation.primaryFailureArtifactSchemaId.empty());
    EXPECT_EQ(validation.primaryFailureArtifactSchemaVersion, 0u);
    EXPECT_TRUE(validation.primaryFailureMessage.empty());
    EXPECT_EQ(validation.checkedPrimaryArtifactCount, 5u);
    EXPECT_EQ(validation.validPrimaryArtifactCount, 5u);
    EXPECT_EQ(validation.failedPrimaryArtifactCount, 0u);
    EXPECT_EQ(validation.actualTotalPrimaryArtifactBytes, artifactResult.totalPrimaryArtifactBytes);
    EXPECT_EQ(validation.expectedPrimaryArtifactBundleHash, artifactResult.primaryArtifactBundleHash);
    EXPECT_EQ(validation.actualPrimaryArtifactBundleHash, artifactResult.primaryArtifactBundleHash);
    EXPECT_TRUE(validation.captureMetadataAvailable);
    EXPECT_EQ(validation.captureId, artifactResult.captureId);
    EXPECT_EQ(validation.captureBaseName, "Frame001");
    EXPECT_EQ(validation.outputDirectory, artifactDirectoryString);
    EXPECT_EQ(validation.frameIndex, toolSnapshot.frame.frameCount);
    EXPECT_EQ(validation.toolDiagnosticsSchemaVersion, RVX_SCENE_RENDERER_TOOL_DIAGNOSTICS_SCHEMA_VERSION);
    EXPECT_EQ(validation.frameDiagnosticsSchemaVersion, RVX_SCENE_RENDERER_FRAME_DIAGNOSTICS_SCHEMA_VERSION);
    EXPECT_EQ(validation.renderGraphDiagnosticsSchemaVersion, RVX_RENDER_GRAPH_DIAGNOSTICS_SCHEMA_VERSION);
    EXPECT_EQ(validation.artifactSummarySchemaVersion,
              RVX_SCENE_RENDERER_TOOL_ARTIFACT_SUMMARY_SCHEMA_VERSION);
    EXPECT_EQ(validation.artifactValidationSchemaVersion,
              RVX_SCENE_RENDERER_TOOL_ARTIFACT_VALIDATION_SCHEMA_VERSION);
    EXPECT_EQ(validation.renderGraphDiagnosticsSchemaId, RVX_RENDER_GRAPH_DIAGNOSTICS_SCHEMA_ID);
    EXPECT_EQ(validation.renderGraphPassCount, toolSnapshot.renderGraph.passes.size());
    EXPECT_EQ(validation.renderGraphResourceCount, toolSnapshot.renderGraph.resources.size());
    ASSERT_EQ(validation.diagnosticCodeCounts.size(), 1u);
    EXPECT_EQ(validation.diagnosticCodeCounts[0].code, "None");
    EXPECT_EQ(validation.diagnosticCodeCounts[0].count, 5u);
    ASSERT_EQ(validation.entries.size(), static_cast<size_t>(5));
    bool renderGraphJsonValidationFound = false;
    for (size_t entryIndex = 0; entryIndex < validation.entries.size(); ++entryIndex)
    {
        const SceneRendererToolDiagnosticsArtifactValidationEntry& entry = validation.entries[entryIndex];
        EXPECT_EQ(entry.entryIndex, static_cast<uint32>(entryIndex));
        EXPECT_TRUE(entry.valid);
        EXPECT_FALSE(entry.primaryFailure);
        EXPECT_TRUE(entry.exists);
        EXPECT_TRUE(entry.byteSizeMatches);
        EXPECT_TRUE(entry.contentHashMatches);
        EXPECT_EQ(entry.diagnosticCode, "None");
        EXPECT_TRUE(entry.diagnosticMessage.empty());
        if (entry.id == "renderGraphDiagnosticsJson")
        {
            renderGraphJsonValidationFound = true;
            EXPECT_EQ(entry.entryIndex, 3u);
            EXPECT_EQ(entry.kind, "RenderGraphDiagnosticsJson");
            EXPECT_EQ(entry.contentType, "application/json");
            EXPECT_EQ(entry.schemaId, RVX_RENDER_GRAPH_DIAGNOSTICS_SCHEMA_ID);
            EXPECT_EQ(entry.schemaVersion, RVX_RENDER_GRAPH_DIAGNOSTICS_SCHEMA_VERSION);
            EXPECT_TRUE(entry.identityChecked);
            EXPECT_TRUE(entry.identityMatches);
            EXPECT_TRUE(entry.schemaChecked);
            EXPECT_TRUE(entry.schemaMatches);
            EXPECT_EQ(entry.actualId, "renderGraphDiagnosticsJson");
            EXPECT_EQ(entry.actualKind, "RenderGraphDiagnosticsJson");
            EXPECT_EQ(entry.actualContentType, "application/json");
            EXPECT_EQ(entry.actualSchemaId, RVX_RENDER_GRAPH_DIAGNOSTICS_SCHEMA_ID);
            EXPECT_EQ(entry.actualSchemaVersion, RVX_RENDER_GRAPH_DIAGNOSTICS_SCHEMA_VERSION);
            EXPECT_EQ(entry.relativePath, "Frame001.rendergraph.json");
            EXPECT_EQ(entry.expectedByteSize, artifactResult.renderGraphDiagnosticsJsonBytes);
            EXPECT_EQ(entry.actualByteSize, artifactResult.renderGraphDiagnosticsJsonBytes);
            EXPECT_EQ(entry.expectedContentHash, artifactResult.renderGraphDiagnosticsJsonContentHash);
            EXPECT_EQ(entry.actualContentHash, artifactResult.renderGraphDiagnosticsJsonContentHash);
        }
    }
    EXPECT_TRUE(renderGraphJsonValidationFound);
    const std::string validationJson =
        SceneRenderer::ExportToolDiagnosticsArtifactValidationJson(validation);
    EXPECT_NE(validationJson.find("\"schemaVersion\": 25"), std::string::npos);
    EXPECT_NE(validationJson.find("\"id\": \"artifactValidationJson\""), std::string::npos);
    EXPECT_NE(validationJson.find("\"kind\": \"ToolDiagnosticsArtifactValidationJson\""),
              std::string::npos);
    EXPECT_NE(validationJson.find("\"contentType\": \"application/json\""), std::string::npos);
    EXPECT_NE(validationJson.find("\"contentHash\": \"\""), std::string::npos);
    EXPECT_NE(validationJson.find("\"relativePath\": \"\""), std::string::npos);
    EXPECT_NE(validationJson.find("\"capture\": {"), std::string::npos);
    EXPECT_NE(validationJson.find("\"metadataAvailable\": true"), std::string::npos);
    EXPECT_NE(validationJson.find("\"captureId\": \"" + artifactResult.captureId + "\""), std::string::npos);
    EXPECT_NE(validationJson.find("\"baseName\": \"Frame001\""), std::string::npos);
    EXPECT_NE(validationJson.find("\"outputDirectory\": "), std::string::npos);
    EXPECT_NE(validationJson.find("\"frameIndex\": " + std::to_string(toolSnapshot.frame.frameCount)),
              std::string::npos);
    EXPECT_NE(validationJson.find(
                  "\"toolDiagnosticsSchemaVersion\": " + std::to_string(
                      RVX_SCENE_RENDERER_TOOL_DIAGNOSTICS_SCHEMA_VERSION)),
              std::string::npos);
    EXPECT_NE(validationJson.find(
                  "\"renderGraphDiagnosticsSchemaVersion\": " +
                  std::to_string(RVX_RENDER_GRAPH_DIAGNOSTICS_SCHEMA_VERSION)),
              std::string::npos);
    EXPECT_NE(validationJson.find("\"renderGraphDiagnosticsSchemaId\": \"RVX.RenderGraph.Diagnostics\""),
              std::string::npos);
    EXPECT_NE(validationJson.find("\"artifactSummarySchemaVersion\": 24"), std::string::npos);
    EXPECT_NE(validationJson.find("\"artifactValidationSchemaVersion\": 25"), std::string::npos);
    EXPECT_NE(validationJson.find("\"renderGraphPassCount\": " +
                                  std::to_string(toolSnapshot.renderGraph.passes.size())),
              std::string::npos);
    EXPECT_NE(validationJson.find("\"renderGraphResourceCount\": " +
                                  std::to_string(toolSnapshot.renderGraph.resources.size())),
              std::string::npos);
    EXPECT_NE(validationJson.find("\"allPrimaryArtifactsValid\": true"), std::string::npos);
    EXPECT_NE(validationJson.find("\"bundleHashMatches\": true"), std::string::npos);
    EXPECT_NE(validationJson.find("\"verdictCode\": \"Valid\""), std::string::npos);
    EXPECT_NE(validationJson.find("\"primaryFailureCode\": \"None\""), std::string::npos);
    EXPECT_NE(validationJson.find("\"primaryFailureEntryIndex\": null"), std::string::npos);
    EXPECT_NE(validationJson.find("\"primaryFailureEntryCount\": 0"), std::string::npos);
    EXPECT_NE(validationJson.find("\"entryCount\": 5"), std::string::npos);
    EXPECT_NE(validationJson.find("\"entryCountMatchesCheckedPrimaryArtifactCount\": true"),
              std::string::npos);
    EXPECT_NE(validationJson.find("\"entryCoverageCode\": \"Complete\""), std::string::npos);
    EXPECT_NE(validationJson.find("\"entryCoverageMessage\": \"validation entries cover all checked primary artifacts\""),
              std::string::npos);
    EXPECT_NE(validationJson.find("\"primaryFailureArtifactId\": \"\""), std::string::npos);
    EXPECT_NE(validationJson.find("\"primaryFailureArtifactRelativePath\": \"\""), std::string::npos);
    EXPECT_NE(validationJson.find("\"primaryFailureArtifactKind\": \"\""), std::string::npos);
    EXPECT_NE(validationJson.find("\"primaryFailureArtifactContentType\": \"\""), std::string::npos);
    EXPECT_NE(validationJson.find("\"primaryFailureArtifactSchemaId\": \"\""), std::string::npos);
    EXPECT_NE(validationJson.find("\"primaryFailureArtifactSchemaVersion\": 0"), std::string::npos);
    EXPECT_NE(validationJson.find("\"primaryFailureMessage\": \"\""), std::string::npos);
    EXPECT_NE(validationJson.find("\"checkedPrimaryArtifactCount\": 5"), std::string::npos);
    EXPECT_NE(validationJson.find("\"validPrimaryArtifactCount\": 5"), std::string::npos);
    EXPECT_NE(validationJson.find("\"failedPrimaryArtifactCount\": 0"), std::string::npos);
    EXPECT_NE(validationJson.find("\"diagnosticCodeCounts\": ["), std::string::npos);
    EXPECT_NE(validationJson.find("\"code\": \"None\""), std::string::npos);
    EXPECT_NE(validationJson.find("\"count\": 5"), std::string::npos);
    EXPECT_NE(validationJson.find("\"expectedPrimaryArtifactBundleHash\": \"" +
                                  artifactResult.primaryArtifactBundleHash + "\""),
              std::string::npos);
    EXPECT_NE(validationJson.find("\"entryIndex\": 3"), std::string::npos);
    EXPECT_NE(validationJson.find("\"primaryFailure\": false"), std::string::npos);
    EXPECT_NE(validationJson.find("\"id\": \"renderGraphDiagnosticsJson\""), std::string::npos);
    EXPECT_NE(validationJson.find("\"kind\": \"RenderGraphDiagnosticsJson\""), std::string::npos);
    EXPECT_NE(validationJson.find("\"contentType\": \"application/json\""), std::string::npos);
    EXPECT_NE(validationJson.find("\"schemaId\": \"RVX.RenderGraph.Diagnostics\""), std::string::npos);
    EXPECT_NE(validationJson.find(
                  "\"schemaVersion\": " + std::to_string(RVX_RENDER_GRAPH_DIAGNOSTICS_SCHEMA_VERSION)),
              std::string::npos);
    EXPECT_NE(validationJson.find("\"actualId\": \"renderGraphDiagnosticsJson\""), std::string::npos);
    EXPECT_NE(validationJson.find("\"actualKind\": \"RenderGraphDiagnosticsJson\""), std::string::npos);
    EXPECT_NE(validationJson.find("\"actualContentType\": \"application/json\""), std::string::npos);
    EXPECT_NE(validationJson.find("\"actualSchemaId\": \"RVX.RenderGraph.Diagnostics\""), std::string::npos);
    EXPECT_NE(validationJson.find(
                  "\"actualSchemaVersion\": " +
                  std::to_string(RVX_RENDER_GRAPH_DIAGNOSTICS_SCHEMA_VERSION)),
              std::string::npos);
    EXPECT_NE(validationJson.find("\"identityChecked\": true"), std::string::npos);
    EXPECT_NE(validationJson.find("\"identityMatches\": true"), std::string::npos);
    EXPECT_NE(validationJson.find("\"schemaChecked\": true"), std::string::npos);
    EXPECT_NE(validationJson.find("\"schemaMatches\": true"), std::string::npos);
    EXPECT_NE(validationJson.find("\"actualContentHash\": \"" +
                                  artifactResult.renderGraphDiagnosticsJsonContentHash + "\""),
              std::string::npos);
    EXPECT_NE(validationJson.find("\"diagnosticCode\": \"None\""), std::string::npos);
    EXPECT_NE(validationJson.find("\"diagnosticMessage\": \"\""), std::string::npos);
    EXPECT_EQ(ReadTextFile(artifactResult.artifactValidationJsonPath), validationJson);
    EXPECT_FALSE(SceneRenderer::SaveToolDiagnosticsArtifactValidationJson(nullptr, validation));
    EXPECT_FALSE(SceneRenderer::SaveToolDiagnosticsArtifactValidationJson("", validation));
    const fs::path validationJsonPath =
        artifactDirectory / "Frame001.diagnostics-validation.json";
    const std::string validationJsonPathString = validationJsonPath.string();
    ASSERT_TRUE(SceneRenderer::SaveToolDiagnosticsArtifactValidationJson(validationJsonPathString.c_str(),
                                                                         validation));
    EXPECT_EQ(ReadTextFile(validationJsonPath), validationJson);

    SceneRendererToolDiagnosticsArtifactResult bundleMismatchArtifact = artifactResult;
    bundleMismatchArtifact.primaryArtifactBundleHash = "0000000000000000";
    const SceneRendererToolDiagnosticsArtifactValidationResult bundleMismatchValidation =
        SceneRenderer::ValidateToolDiagnosticsArtifacts(bundleMismatchArtifact);
    EXPECT_FALSE(bundleMismatchValidation.allPrimaryArtifactsValid);
    EXPECT_FALSE(bundleMismatchValidation.bundleHashMatches);
    EXPECT_EQ(bundleMismatchValidation.validPrimaryArtifactCount,
              bundleMismatchValidation.checkedPrimaryArtifactCount);
    EXPECT_EQ(bundleMismatchValidation.failedPrimaryArtifactCount, 0u);
    EXPECT_EQ(bundleMismatchValidation.verdictCode, "BundleHashMismatch");
    EXPECT_EQ(bundleMismatchValidation.primaryFailureCode, "BundleHashMismatch");
    EXPECT_EQ(bundleMismatchValidation.primaryFailureEntryIndex, RVX_INVALID_INDEX);
    EXPECT_EQ(bundleMismatchValidation.primaryFailureEntryCount, 0u);
    EXPECT_EQ(bundleMismatchValidation.entryCount, 5u);
    EXPECT_TRUE(bundleMismatchValidation.entryCountMatchesCheckedPrimaryArtifactCount);
    EXPECT_EQ(bundleMismatchValidation.entryCoverageCode, "Complete");
    EXPECT_EQ(bundleMismatchValidation.entryCoverageMessage,
              "validation entries cover all checked primary artifacts");
    EXPECT_TRUE(bundleMismatchValidation.primaryFailureArtifactId.empty());
    EXPECT_TRUE(bundleMismatchValidation.primaryFailureArtifactRelativePath.empty());
    EXPECT_TRUE(bundleMismatchValidation.primaryFailureArtifactKind.empty());
    EXPECT_TRUE(bundleMismatchValidation.primaryFailureArtifactContentType.empty());
    EXPECT_TRUE(bundleMismatchValidation.primaryFailureArtifactSchemaId.empty());
    EXPECT_EQ(bundleMismatchValidation.primaryFailureArtifactSchemaVersion, 0u);
    EXPECT_EQ(bundleMismatchValidation.primaryFailureMessage, "primary artifact bundle hash mismatch");
    const std::string bundleMismatchJson =
        SceneRenderer::ExportToolDiagnosticsArtifactValidationJson(bundleMismatchValidation);
    EXPECT_NE(bundleMismatchJson.find("\"bundleHashMatches\": false"), std::string::npos);
    EXPECT_NE(bundleMismatchJson.find("\"verdictCode\": \"BundleHashMismatch\""), std::string::npos);
    EXPECT_NE(bundleMismatchJson.find("\"primaryFailureCode\": \"BundleHashMismatch\""),
              std::string::npos);
    EXPECT_NE(bundleMismatchJson.find("\"primaryFailureEntryIndex\": null"), std::string::npos);
    EXPECT_NE(bundleMismatchJson.find("\"primaryFailureEntryCount\": 0"), std::string::npos);
    EXPECT_NE(bundleMismatchJson.find("\"entryCount\": 5"), std::string::npos);
    EXPECT_NE(bundleMismatchJson.find("\"entryCountMatchesCheckedPrimaryArtifactCount\": true"),
              std::string::npos);
    EXPECT_NE(bundleMismatchJson.find("\"entryCoverageCode\": \"Complete\""), std::string::npos);
    EXPECT_NE(bundleMismatchJson.find("\"entryCoverageMessage\": \"validation entries cover all checked primary artifacts\""),
              std::string::npos);
    EXPECT_NE(bundleMismatchJson.find("\"primaryFailureArtifactId\": \"\""), std::string::npos);
    EXPECT_NE(bundleMismatchJson.find("\"primaryFailureArtifactRelativePath\": \"\""),
              std::string::npos);
    EXPECT_NE(bundleMismatchJson.find("\"primaryFailureArtifactKind\": \"\""), std::string::npos);
    EXPECT_NE(bundleMismatchJson.find("\"primaryFailureArtifactContentType\": \"\""),
              std::string::npos);
    EXPECT_NE(bundleMismatchJson.find("\"primaryFailureArtifactSchemaId\": \"\""),
              std::string::npos);
    EXPECT_NE(bundleMismatchJson.find("\"primaryFailureArtifactSchemaVersion\": 0"),
              std::string::npos);
    EXPECT_NE(bundleMismatchJson.find("\"primaryFailureMessage\": \"primary artifact bundle hash mismatch\""),
              std::string::npos);
    EXPECT_NE(bundleMismatchJson.find("\"failedPrimaryArtifactCount\": 0"), std::string::npos);
    for (const SceneRendererToolDiagnosticsArtifactValidationEntry& entry : bundleMismatchValidation.entries)
    {
        EXPECT_FALSE(entry.primaryFailure);
    }

    std::string wrongIdentityJson = renderGraphDiagnosticsJson;
    const std::string expectedIdentity = "\"id\": \"renderGraphDiagnosticsJson\"";
    const std::string wrongIdentity = "\"id\": \"unexpectedRenderGraphDiagnosticsJson\"";
    const size_t identityOffset = wrongIdentityJson.find(expectedIdentity);
    ASSERT_NE(identityOffset, std::string::npos);
    wrongIdentityJson.replace(identityOffset, expectedIdentity.size(), wrongIdentity);
    {
        std::ofstream wrongIdentityArtifact(artifactResult.renderGraphDiagnosticsJsonPath,
                                            std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(wrongIdentityArtifact.is_open());
        wrongIdentityArtifact << wrongIdentityJson;
    }

    SceneRendererToolDiagnosticsArtifactResult identityMismatchArtifact = artifactResult;
    identityMismatchArtifact.renderGraphDiagnosticsJsonBytes =
        static_cast<uint64>(wrongIdentityJson.size());
    identityMismatchArtifact.renderGraphDiagnosticsJsonContentHash =
        ComputeTestContentHash(wrongIdentityJson);
    const SceneRendererToolDiagnosticsArtifactValidationResult identityMismatchValidation =
        SceneRenderer::ValidateToolDiagnosticsArtifacts(identityMismatchArtifact);
    EXPECT_FALSE(identityMismatchValidation.allPrimaryArtifactsValid);
    EXPECT_EQ(identityMismatchValidation.verdictCode, "InvalidArtifacts");
    EXPECT_EQ(identityMismatchValidation.checkedPrimaryArtifactCount, 5u);
    EXPECT_EQ(identityMismatchValidation.validPrimaryArtifactCount, 4u);
    EXPECT_EQ(identityMismatchValidation.failedPrimaryArtifactCount, 1u);
    EXPECT_EQ(identityMismatchValidation.primaryFailureCode, "IdentityMetadataMismatch");
    EXPECT_EQ(identityMismatchValidation.primaryFailureEntryIndex, 3u);
    EXPECT_EQ(identityMismatchValidation.primaryFailureEntryCount, 1u);
    EXPECT_EQ(identityMismatchValidation.entryCount, 5u);
    EXPECT_TRUE(identityMismatchValidation.entryCountMatchesCheckedPrimaryArtifactCount);
    EXPECT_EQ(identityMismatchValidation.entryCoverageCode, "Complete");
    EXPECT_EQ(identityMismatchValidation.entryCoverageMessage,
              "validation entries cover all checked primary artifacts");
    EXPECT_EQ(identityMismatchValidation.primaryFailureArtifactId, "renderGraphDiagnosticsJson");
    EXPECT_EQ(identityMismatchValidation.primaryFailureArtifactRelativePath, "Frame001.rendergraph.json");
    EXPECT_EQ(identityMismatchValidation.primaryFailureArtifactKind, "RenderGraphDiagnosticsJson");
    EXPECT_EQ(identityMismatchValidation.primaryFailureArtifactContentType, "application/json");
    EXPECT_EQ(identityMismatchValidation.primaryFailureArtifactSchemaId, RVX_RENDER_GRAPH_DIAGNOSTICS_SCHEMA_ID);
    EXPECT_EQ(identityMismatchValidation.primaryFailureArtifactSchemaVersion,
              RVX_RENDER_GRAPH_DIAGNOSTICS_SCHEMA_VERSION);
    EXPECT_EQ(identityMismatchValidation.primaryFailureMessage, "artifact identity metadata mismatch");
    bool identityMismatchCodeCountFound = false;
    bool identityMismatchNoneCodeCountFound = false;
    for (const SceneRendererToolDiagnosticsArtifactValidationCodeCount& codeCount :
         identityMismatchValidation.diagnosticCodeCounts)
    {
        if (codeCount.code == "IdentityMetadataMismatch")
        {
            identityMismatchCodeCountFound = true;
            EXPECT_EQ(codeCount.count, 1u);
        }
        else if (codeCount.code == "None")
        {
            identityMismatchNoneCodeCountFound = true;
            EXPECT_EQ(codeCount.count, 4u);
        }
    }
    EXPECT_TRUE(identityMismatchCodeCountFound);
    EXPECT_TRUE(identityMismatchNoneCodeCountFound);
    bool renderGraphJsonIdentityMismatchFound = false;
    for (const SceneRendererToolDiagnosticsArtifactValidationEntry& entry : identityMismatchValidation.entries)
    {
        if (entry.id == "renderGraphDiagnosticsJson")
        {
            renderGraphJsonIdentityMismatchFound = true;
            EXPECT_EQ(entry.entryIndex, 3u);
            EXPECT_TRUE(entry.primaryFailure);
            EXPECT_FALSE(entry.valid);
            EXPECT_TRUE(entry.exists);
            EXPECT_TRUE(entry.byteSizeMatches);
            EXPECT_TRUE(entry.contentHashMatches);
            EXPECT_TRUE(entry.identityChecked);
            EXPECT_FALSE(entry.identityMatches);
            EXPECT_TRUE(entry.schemaChecked);
            EXPECT_TRUE(entry.schemaMatches);
            EXPECT_EQ(entry.actualId, "unexpectedRenderGraphDiagnosticsJson");
            EXPECT_EQ(entry.actualKind, "RenderGraphDiagnosticsJson");
            EXPECT_EQ(entry.actualContentType, "application/json");
            EXPECT_EQ(entry.actualSchemaId, RVX_RENDER_GRAPH_DIAGNOSTICS_SCHEMA_ID);
            EXPECT_EQ(entry.actualSchemaVersion, RVX_RENDER_GRAPH_DIAGNOSTICS_SCHEMA_VERSION);
            EXPECT_EQ(entry.diagnosticCode, "IdentityMetadataMismatch");
            EXPECT_EQ(entry.diagnosticMessage, "artifact identity metadata mismatch");
        }
    }
    EXPECT_TRUE(renderGraphJsonIdentityMismatchFound);
    const std::string identityMismatchJson =
        SceneRenderer::ExportToolDiagnosticsArtifactValidationJson(identityMismatchValidation);
    EXPECT_NE(identityMismatchJson.find("\"identityMatches\": false"), std::string::npos);
    EXPECT_NE(identityMismatchJson.find("\"schemaMatches\": true"), std::string::npos);
    EXPECT_NE(identityMismatchJson.find("\"actualId\": \"unexpectedRenderGraphDiagnosticsJson\""),
              std::string::npos);
    EXPECT_NE(identityMismatchJson.find("\"actualKind\": \"RenderGraphDiagnosticsJson\""), std::string::npos);
    EXPECT_NE(identityMismatchJson.find(
                  "\"actualSchemaVersion\": " +
                  std::to_string(RVX_RENDER_GRAPH_DIAGNOSTICS_SCHEMA_VERSION)),
              std::string::npos);
    EXPECT_NE(identityMismatchJson.find("\"verdictCode\": \"InvalidArtifacts\""), std::string::npos);
    EXPECT_NE(identityMismatchJson.find("\"primaryFailureCode\": \"IdentityMetadataMismatch\""),
              std::string::npos);
    EXPECT_NE(identityMismatchJson.find("\"primaryFailureEntryIndex\": 3"), std::string::npos);
    EXPECT_NE(identityMismatchJson.find("\"primaryFailureEntryCount\": 1"), std::string::npos);
    EXPECT_NE(identityMismatchJson.find("\"entryCount\": 5"), std::string::npos);
    EXPECT_NE(identityMismatchJson.find("\"entryCountMatchesCheckedPrimaryArtifactCount\": true"),
              std::string::npos);
    EXPECT_NE(identityMismatchJson.find("\"entryCoverageCode\": \"Complete\""), std::string::npos);
    EXPECT_NE(identityMismatchJson.find("\"entryCoverageMessage\": \"validation entries cover all checked primary artifacts\""),
              std::string::npos);
    EXPECT_NE(identityMismatchJson.find("\"primaryFailureArtifactId\": \"renderGraphDiagnosticsJson\""),
              std::string::npos);
    EXPECT_NE(identityMismatchJson.find("\"primaryFailureArtifactRelativePath\": \"Frame001.rendergraph.json\""),
              std::string::npos);
    EXPECT_NE(identityMismatchJson.find("\"primaryFailureArtifactKind\": \"RenderGraphDiagnosticsJson\""),
              std::string::npos);
    EXPECT_NE(identityMismatchJson.find("\"primaryFailureArtifactContentType\": \"application/json\""),
              std::string::npos);
    EXPECT_NE(identityMismatchJson.find("\"primaryFailureArtifactSchemaId\": \"RVX.RenderGraph.Diagnostics\""),
              std::string::npos);
    EXPECT_NE(identityMismatchJson.find(
                  "\"primaryFailureArtifactSchemaVersion\": " +
                  std::to_string(RVX_RENDER_GRAPH_DIAGNOSTICS_SCHEMA_VERSION)),
              std::string::npos);
    EXPECT_NE(identityMismatchJson.find("\"primaryFailureMessage\": \"artifact identity metadata mismatch\""),
              std::string::npos);
    EXPECT_NE(identityMismatchJson.find("\"failedPrimaryArtifactCount\": 1"), std::string::npos);
    EXPECT_NE(identityMismatchJson.find("\"entryIndex\": 3"), std::string::npos);
    EXPECT_NE(identityMismatchJson.find("\"primaryFailure\": true"), std::string::npos);
    EXPECT_NE(identityMismatchJson.find("\"code\": \"IdentityMetadataMismatch\""),
              std::string::npos);
    EXPECT_NE(identityMismatchJson.find("\"count\": 1"), std::string::npos);
    EXPECT_NE(identityMismatchJson.find("\"diagnosticCode\": \"IdentityMetadataMismatch\""),
              std::string::npos);
    EXPECT_NE(identityMismatchJson.find("\"diagnosticMessage\": \"artifact identity metadata mismatch\""),
              std::string::npos);

    {
        std::ofstream restoredArtifact(artifactResult.renderGraphDiagnosticsJsonPath,
                                       std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(restoredArtifact.is_open());
        restoredArtifact << renderGraphDiagnosticsJson;
    }

    {
        std::ofstream modifiedArtifact(artifactResult.renderGraphDiagnosticsJsonPath,
                                       std::ios::binary | std::ios::app);
        ASSERT_TRUE(modifiedArtifact.is_open());
        modifiedArtifact << "\n";
    }

    const SceneRendererToolDiagnosticsArtifactValidationResult modifiedValidation =
        SceneRenderer::ValidateToolDiagnosticsArtifacts(artifactResult);
    EXPECT_FALSE(modifiedValidation.allPrimaryArtifactsValid);
    EXPECT_FALSE(modifiedValidation.bundleHashMatches);
    EXPECT_EQ(modifiedValidation.verdictCode, "InvalidArtifacts");
    EXPECT_EQ(modifiedValidation.checkedPrimaryArtifactCount, 5u);
    EXPECT_EQ(modifiedValidation.validPrimaryArtifactCount, 4u);
    EXPECT_EQ(modifiedValidation.failedPrimaryArtifactCount, 1u);
    EXPECT_EQ(modifiedValidation.primaryFailureCode, "ByteSizeMismatch");
    EXPECT_EQ(modifiedValidation.primaryFailureEntryIndex, 3u);
    EXPECT_EQ(modifiedValidation.primaryFailureEntryCount, 1u);
    EXPECT_EQ(modifiedValidation.entryCount, 5u);
    EXPECT_TRUE(modifiedValidation.entryCountMatchesCheckedPrimaryArtifactCount);
    EXPECT_EQ(modifiedValidation.entryCoverageCode, "Complete");
    EXPECT_EQ(modifiedValidation.entryCoverageMessage,
              "validation entries cover all checked primary artifacts");
    EXPECT_EQ(modifiedValidation.primaryFailureArtifactId, "renderGraphDiagnosticsJson");
    EXPECT_EQ(modifiedValidation.primaryFailureArtifactRelativePath, "Frame001.rendergraph.json");
    EXPECT_EQ(modifiedValidation.primaryFailureArtifactKind, "RenderGraphDiagnosticsJson");
    EXPECT_EQ(modifiedValidation.primaryFailureArtifactContentType, "application/json");
    EXPECT_EQ(modifiedValidation.primaryFailureArtifactSchemaId, RVX_RENDER_GRAPH_DIAGNOSTICS_SCHEMA_ID);
    EXPECT_EQ(modifiedValidation.primaryFailureArtifactSchemaVersion,
              RVX_RENDER_GRAPH_DIAGNOSTICS_SCHEMA_VERSION);
    EXPECT_EQ(modifiedValidation.primaryFailureMessage, "artifact byte size mismatch");
    bool byteSizeMismatchCodeCountFound = false;
    bool byteSizeMismatchNoneCodeCountFound = false;
    for (const SceneRendererToolDiagnosticsArtifactValidationCodeCount& codeCount :
         modifiedValidation.diagnosticCodeCounts)
    {
        if (codeCount.code == "ByteSizeMismatch")
        {
            byteSizeMismatchCodeCountFound = true;
            EXPECT_EQ(codeCount.count, 1u);
        }
        else if (codeCount.code == "None")
        {
            byteSizeMismatchNoneCodeCountFound = true;
            EXPECT_EQ(codeCount.count, 4u);
        }
    }
    EXPECT_TRUE(byteSizeMismatchCodeCountFound);
    EXPECT_TRUE(byteSizeMismatchNoneCodeCountFound);
    bool renderGraphJsonMismatchFound = false;
    for (const SceneRendererToolDiagnosticsArtifactValidationEntry& entry : modifiedValidation.entries)
    {
        if (entry.id == "renderGraphDiagnosticsJson")
        {
            renderGraphJsonMismatchFound = true;
            EXPECT_EQ(entry.entryIndex, 3u);
            EXPECT_TRUE(entry.primaryFailure);
            EXPECT_FALSE(entry.valid);
            EXPECT_TRUE(entry.exists);
            EXPECT_FALSE(entry.byteSizeMatches);
            EXPECT_FALSE(entry.contentHashMatches);
            EXPECT_FALSE(entry.diagnosticMessage.empty());
            EXPECT_EQ(entry.diagnosticCode, "ByteSizeMismatch");
            EXPECT_NE(entry.actualByteSize, entry.expectedByteSize);
            EXPECT_NE(entry.actualContentHash, entry.expectedContentHash);
        }
    }
    EXPECT_TRUE(renderGraphJsonMismatchFound);
    const std::string modifiedValidationJson =
        SceneRenderer::ExportToolDiagnosticsArtifactValidationJson(modifiedValidation);
    EXPECT_NE(modifiedValidationJson.find("\"allPrimaryArtifactsValid\": false"), std::string::npos);
    EXPECT_NE(modifiedValidationJson.find("\"bundleHashMatches\": false"), std::string::npos);
    EXPECT_NE(modifiedValidationJson.find("\"verdictCode\": \"InvalidArtifacts\""), std::string::npos);
    EXPECT_NE(modifiedValidationJson.find("\"primaryFailureCode\": \"ByteSizeMismatch\""),
              std::string::npos);
    EXPECT_NE(modifiedValidationJson.find("\"primaryFailureEntryIndex\": 3"), std::string::npos);
    EXPECT_NE(modifiedValidationJson.find("\"primaryFailureEntryCount\": 1"), std::string::npos);
    EXPECT_NE(modifiedValidationJson.find("\"entryCount\": 5"), std::string::npos);
    EXPECT_NE(modifiedValidationJson.find("\"entryCountMatchesCheckedPrimaryArtifactCount\": true"),
              std::string::npos);
    EXPECT_NE(modifiedValidationJson.find("\"entryCoverageCode\": \"Complete\""), std::string::npos);
    EXPECT_NE(modifiedValidationJson.find("\"entryCoverageMessage\": \"validation entries cover all checked primary artifacts\""),
              std::string::npos);
    EXPECT_NE(modifiedValidationJson.find("\"primaryFailureArtifactId\": \"renderGraphDiagnosticsJson\""),
              std::string::npos);
    EXPECT_NE(modifiedValidationJson.find("\"primaryFailureArtifactRelativePath\": \"Frame001.rendergraph.json\""),
              std::string::npos);
    EXPECT_NE(modifiedValidationJson.find("\"primaryFailureArtifactKind\": \"RenderGraphDiagnosticsJson\""),
              std::string::npos);
    EXPECT_NE(modifiedValidationJson.find("\"primaryFailureArtifactContentType\": \"application/json\""),
              std::string::npos);
    EXPECT_NE(modifiedValidationJson.find("\"primaryFailureArtifactSchemaId\": \"RVX.RenderGraph.Diagnostics\""),
              std::string::npos);
    EXPECT_NE(modifiedValidationJson.find(
                  "\"primaryFailureArtifactSchemaVersion\": " +
                  std::to_string(RVX_RENDER_GRAPH_DIAGNOSTICS_SCHEMA_VERSION)),
              std::string::npos);
    EXPECT_NE(modifiedValidationJson.find("\"primaryFailureMessage\": \"artifact byte size mismatch\""),
              std::string::npos);
    EXPECT_NE(modifiedValidationJson.find("\"failedPrimaryArtifactCount\": 1"), std::string::npos);
    EXPECT_NE(modifiedValidationJson.find("\"entryIndex\": 3"), std::string::npos);
    EXPECT_NE(modifiedValidationJson.find("\"primaryFailure\": true"), std::string::npos);
    EXPECT_NE(modifiedValidationJson.find("\"id\": \"renderGraphDiagnosticsJson\""), std::string::npos);
    EXPECT_NE(modifiedValidationJson.find("\"valid\": false"), std::string::npos);
    EXPECT_NE(modifiedValidationJson.find("\"byteSizeMatches\": false"), std::string::npos);
    EXPECT_NE(modifiedValidationJson.find("\"contentHashMatches\": false"), std::string::npos);
    EXPECT_NE(modifiedValidationJson.find("\"code\": \"ByteSizeMismatch\""),
              std::string::npos);
    EXPECT_NE(modifiedValidationJson.find("\"count\": 1"), std::string::npos);
    EXPECT_NE(modifiedValidationJson.find("\"diagnosticCode\": \"ByteSizeMismatch\""),
              std::string::npos);
    EXPECT_NE(modifiedValidationJson.find("\"diagnosticMessage\": \"artifact byte size mismatch\""),
              std::string::npos);
    fs::remove_all(artifactDirectory, removeError);
}

TEST(SceneRendererDiagnosticsValidation, RenderFeatureReportMapsModernFeaturesToCapabilities)
{
    FakeDevice device;
    SceneRenderer renderer;
    renderer.SetRenderFeatureReportDeviceForTesting(&device);

    auto graph = std::make_unique<RenderGraph>();
    graph->SetDevice(&device);
    renderer.SetRenderGraphForTesting(std::move(graph));

    renderer.AddPass(std::make_unique<StatusTestPass>("ShadowPass",
                                                      200,
                                                      true,
                                                      false,
                                                      "Shadow atlas unavailable"));
    renderer.AddPass(std::make_unique<StatusTestPass>("OpaquePass", 300, true, true));
    renderer.AddPass(std::make_unique<StatusTestPass>("RayTracedShadowPass",
                                                      350,
                                                      true,
                                                      true));

    renderer.RefreshFrameDiagnosticsForTesting();

    const SceneRenderFeatureReport& report = renderer.GetRenderFeatureReport();
    EXPECT_EQ(report.schemaVersion, RVX_SCENE_RENDER_FEATURE_REPORT_SCHEMA_VERSION);
    EXPECT_EQ(report.features.size(), static_cast<size_t>(7));
    EXPECT_EQ(GetSceneRenderFeatureName(SceneRenderFeature::GPUDriven), std::string("GPUDriven"));
    EXPECT_EQ(GetSceneRenderFeatureStatusName(SceneRenderFeatureStatus::Fallback), std::string("Fallback"));
    EXPECT_GE(report.supportedCount, 1u);
    EXPECT_GE(report.fallbackCount, 1u);
    EXPECT_GE(report.unsupportedCount, 1u);
    EXPECT_GE(report.skippedCount, 1u);

    const SceneRenderFeatureCapability* pbr =
        FindRenderFeature(report, SceneRenderFeature::PBR);
    ASSERT_NE(pbr, nullptr);
    EXPECT_EQ(pbr->status, SceneRenderFeatureStatus::Supported);
    EXPECT_TRUE(pbr->requested);
    EXPECT_TRUE(pbr->supported);
    EXPECT_TRUE(pbr->enabled);
    EXPECT_TRUE(pbr->renderGraphBacked);
    EXPECT_EQ(pbr->requiredCapability, "graphicsPipeline+materialPipeline");

    const SceneRenderFeatureCapability* shadows =
        FindRenderFeature(report, SceneRenderFeature::Shadows);
    ASSERT_NE(shadows, nullptr);
    EXPECT_EQ(shadows->status, SceneRenderFeatureStatus::Fallback);
    EXPECT_TRUE(shadows->requested);
    EXPECT_FALSE(shadows->supported);
    EXPECT_TRUE(shadows->fallbackUsed);
    EXPECT_NE(shadows->diagnosticMessage.find("Shadow atlas unavailable"), std::string::npos);

    const SceneRenderFeatureCapability* rayTracing =
        FindRenderFeature(report, SceneRenderFeature::RayTracing);
    ASSERT_NE(rayTracing, nullptr);
    EXPECT_EQ(rayTracing->status, SceneRenderFeatureStatus::Unsupported);
    EXPECT_TRUE(rayTracing->requested);
    EXPECT_TRUE(rayTracing->rhiCapabilityKnown);
    EXPECT_FALSE(rayTracing->supported);
    EXPECT_EQ(rayTracing->requiredCapability, "supportsRaytracing+supportsRaytracingPipeline");

    const SceneRenderFeatureCapability* postProcess =
        FindRenderFeature(report, SceneRenderFeature::PostProcess);
    ASSERT_NE(postProcess, nullptr);
    EXPECT_EQ(postProcess->status, SceneRenderFeatureStatus::Skipped);
    EXPECT_FALSE(postProcess->requested);

    const std::string diagnosticsText = renderer.ExportToolDiagnosticsText();
    EXPECT_NE(diagnosticsText.find("RenderFeatures: supported="), std::string::npos);
    EXPECT_NE(diagnosticsText.find("Feature PBR: status=Supported"), std::string::npos);
    EXPECT_NE(diagnosticsText.find("Feature Shadows: status=Fallback"), std::string::npos);
    EXPECT_NE(diagnosticsText.find("Feature RayTracing: status=Unsupported"), std::string::npos);

    const std::string manifestJson = renderer.ExportToolDiagnosticsManifestJson();
    EXPECT_NE(manifestJson.find("\"features\": {"), std::string::npos);
    EXPECT_NE(manifestJson.find("\"name\": \"RayTracing\""), std::string::npos);
    EXPECT_NE(manifestJson.find("\"status\": \"Unsupported\""), std::string::npos);
    EXPECT_NE(manifestJson.find("\"requiredCapability\": \"supportsRaytracing+supportsRaytracingPipeline\""),
              std::string::npos);
}

TEST_F(RenderPassValidationFixture, FilmGrainRequiresResourcesBeforeReportingSupported)
{
    FilmGrainPass pass;
    PostProcessSettings settings;
    settings.enableFilmGrain = true;
    pass.Configure(settings);

    EXPECT_TRUE(pass.IsRequestedEnabled());
    EXPECT_FALSE(pass.IsSupported());
    EXPECT_FALSE(pass.IsEnabled());
    EXPECT_FALSE(pass.GetUnsupportedReason().empty());
}

TEST_F(RenderPassValidationFixture, FilmGrainAddsLiveGraphPassAndDrawsFullscreenTriangle)
{
    RVX_REQUIRE_RENDER_RUNTIME_PIPELINE();

    FilmGrainPass pass;
    PostProcessSettings settings;
    settings.enableFilmGrain = true;
    settings.filmGrainIntensity = 0.2f;
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
    EXPECT_EQ(ctx.pipelineSequence[0], pipelineCache.GetFilmGrainPipeline(RHIFormat::RGBA8_UNORM));
    ASSERT_EQ(ctx.descriptorSetSequence.size(), static_cast<size_t>(1));
    EXPECT_EQ(ctx.descriptorSetSequence[0], 0u);
    EXPECT_EQ(ctx.drawCount, 1u);
    EXPECT_EQ(ctx.lastDrawVertexCount, 3u);
    EXPECT_EQ(ctx.drawIndexedCount, 0u);

    auto descriptorIt = std::find_if(
        device.createdDescriptorSetDescs.begin(),
        device.createdDescriptorSetDescs.end(),
        [](const RHIDescriptorSetDesc& desc)
        {
            return desc.debugName && std::string(desc.debugName) == "FilmGrainDescriptorSet";
        });
    ASSERT_NE(descriptorIt, device.createdDescriptorSetDescs.end());
    EXPECT_EQ(descriptorIt->layout, pipelineCache.GetPostProcessSetLayout());
    ASSERT_EQ(descriptorIt->bindings.size(), static_cast<size_t>(4));
    EXPECT_EQ(descriptorIt->bindings[3].binding, 3u);
    EXPECT_EQ(descriptorIt->bindings[3].textureView, descriptorIt->bindings[1].textureView);
}

TEST_F(RenderPassValidationFixture, FilmGrainUploadsConstantsWithHLSLPacking)
{
    RVX_REQUIRE_RENDER_RUNTIME_PIPELINE();

    FilmGrainPass pass;
    PostProcessSettings settings;
    settings.enableFilmGrain = true;
    pass.Configure(settings);

    FilmGrainConfig config;
    config.type = FilmGrainType::Colored;
    config.intensity = 0.42f;
    config.response = 0.65f;
    config.size = 2.25f;
    config.luminanceContribution = 0.75f;
    config.colorContribution = 0.25f;
    config.animated = true;
    config.animationSpeed = 2.0f;
    pass.SetConfig(config);
    pass.SetFrameTime(1.5f);
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

    const FakeBuffer* constants = FindCreatedBuffer(device, "FilmGrainConstants");
    ASSERT_NE(constants, nullptr);
    const std::vector<uint8>& storage = constants->GetStorage();
    ASSERT_GE(storage.size(), static_cast<size_t>(48));

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
    EXPECT_FLOAT_EQ(readFloat(20), config.response);
    EXPECT_FLOAT_EQ(readFloat(24), config.size);
    EXPECT_FLOAT_EQ(readFloat(28), config.luminanceContribution);
    EXPECT_FLOAT_EQ(readFloat(32), config.colorContribution);
    EXPECT_FLOAT_EQ(readFloat(36), 3.0f);
    EXPECT_FLOAT_EQ(readFloat(40), static_cast<float>(static_cast<uint32>(config.type)));
    EXPECT_FLOAT_EQ(readFloat(44), 0.0f);
}

TEST_F(RenderPassValidationFixture, FilmGrainSkipsDrawWhenConstantsCannotMap)
{
    RVX_REQUIRE_RENDER_RUNTIME_PIPELINE(false);

    FilmGrainPass pass;
    PostProcessSettings settings;
    settings.enableFilmGrain = true;
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

TEST_F(RenderPassValidationFixture, SSAOPassRequiresResourcesBeforeReportingSupported)
{
    SSAOPass pass;
    PostProcessSettings settings;
    settings.enableSSAO = true;
    pass.Configure(settings);

    EXPECT_TRUE(pass.IsRequestedEnabled());
    EXPECT_FALSE(pass.IsSupported());
    EXPECT_FALSE(pass.IsEnabled());
    EXPECT_FALSE(pass.GetUnsupportedReason().empty());
}

TEST_F(RenderPassValidationFixture, SSAOPassAddsDepthOnlyGraphPassAndDrawsFullscreenTriangle)
{
    RVX_REQUIRE_RENDER_RUNTIME_PIPELINE();

    SSAOPass pass;
    PostProcessSettings settings;
    settings.enableSSAO = true;
    settings.ssaoRadius = 0.75f;
    settings.ssaoIntensity = 0.6f;
    settings.visualQualityPreset = RenderVisualQualityPreset::High;
    pass.Configure(settings);
    pass.SetResources(&pipelineCache, &viewCache);

    ASSERT_TRUE(pass.IsSupported()) << pass.GetUnsupportedReason();
    ASSERT_TRUE(pass.IsEnabled());

    RenderGraph graph;
    graph.SetDevice(&device);

    RHITextureDesc inputDesc = RHITextureDesc::RenderTarget(64, 64, RHIFormat::RGBA16_FLOAT);
    inputDesc.usage = RHITextureUsage::RenderTarget | RHITextureUsage::ShaderResource;
    RHITextureDesc outputDesc = RHITextureDesc::RenderTarget(64, 64, RHIFormat::RGBA16_FLOAT);
    outputDesc.usage = RHITextureUsage::RenderTarget | RHITextureUsage::ShaderResource;
    RHITextureDesc depthDesc = RHITextureDesc::DepthStencil(64, 64, RHIFormat::D32_FLOAT);
    depthDesc.usage = RHITextureUsage::DepthStencil | RHITextureUsage::ShaderResource;

    RHITextureRef inputTexture = device.CreateTexture(inputDesc);
    RHITextureRef outputTexture = device.CreateTexture(outputDesc);
    RHITextureRef depthTexture = device.CreateTexture(depthDesc);
    ASSERT_TRUE(inputTexture);
    ASSERT_TRUE(outputTexture);
    ASSERT_TRUE(depthTexture);

    PostProcessFrameInputs frameInputs;
    frameInputs.sceneColor = graph.ImportTexture(inputTexture.Get(), RHIResourceState::ShaderResource);
    frameInputs.depth = graph.ImportTexture(depthTexture.Get(), RHIResourceState::DepthWrite);
    frameInputs.outputFormat = RHIFormat::RGBA16_FLOAT;
    RGTextureHandle output = graph.ImportTexture(outputTexture.Get(), RHIResourceState::RenderTarget);
    graph.SetExportState(output, RHIResourceState::RenderTarget);

    pass.AddToGraph(graph, frameInputs, output);
    graph.Compile();

    const auto& graphStats = graph.GetCompileStats();
    EXPECT_TRUE(graphStats.compileValid);
    EXPECT_EQ(graphStats.totalPasses, 1u);
    EXPECT_EQ(graphStats.culledPasses, 0u);
    EXPECT_EQ(graphStats.emptyPassUsageCount, 0u);

    RecordingCommandContext ctx;
    graph.Execute(ctx);

    EXPECT_EQ(ctx.beginRenderPassCount, 1u);
    EXPECT_EQ(ctx.endRenderPassCount, 1u);
    ASSERT_EQ(ctx.pipelineSequence.size(), static_cast<size_t>(1));
    EXPECT_EQ(ctx.pipelineSequence[0], pipelineCache.GetSSAOPipeline(RHIFormat::RGBA16_FLOAT));
    ASSERT_EQ(ctx.descriptorSetSequence.size(), static_cast<size_t>(1));
    EXPECT_EQ(ctx.descriptorSetSequence[0], 0u);
    EXPECT_EQ(ctx.drawCount, 1u);
    EXPECT_EQ(ctx.lastDrawVertexCount, 3u);
    EXPECT_EQ(ctx.drawIndexedCount, 0u);

    auto descriptorIt = std::find_if(
        device.createdDescriptorSetDescs.begin(),
        device.createdDescriptorSetDescs.end(),
        [](const RHIDescriptorSetDesc& desc)
        {
            return desc.debugName && std::string(desc.debugName) == "SSAODescriptorSet";
        });
    ASSERT_NE(descriptorIt, device.createdDescriptorSetDescs.end());
    EXPECT_EQ(descriptorIt->layout, pipelineCache.GetPostProcessSetLayout());
    ASSERT_EQ(descriptorIt->bindings.size(), static_cast<size_t>(4));

    const SSAOComputeStats& stats = pass.GetLastGraphStats();
    EXPECT_TRUE(stats.requested);
    EXPECT_TRUE(stats.supported);
    EXPECT_TRUE(stats.executed);
    EXPECT_TRUE(stats.depthAvailable);
    EXPECT_FALSE(stats.normalAvailable);
    EXPECT_TRUE(stats.normalFallbackUsed);
    EXPECT_FALSE(stats.neutralOutputFallbackUsed);
    EXPECT_EQ(stats.sampleCount, 12u);
    EXPECT_EQ(stats.aoPassCount, 1u);
    EXPECT_EQ(stats.blurPassCount, 0u);
    EXPECT_EQ(stats.implementationTier, SSAOImplementationTier::DepthOnlyLowTier);
    EXPECT_NE(stats.fallbackReason.find("depth-only low-tier fallback"), std::string::npos);
}

TEST(RenderPostProcessStackValidation, RenderVisualQualityPresetAppliesExplicitEffectPolicy)
{
    PostProcessSettings settings;
    ApplyRenderVisualQualityPreset(settings, RenderVisualQualityPreset::Off);
    EXPECT_EQ(settings.visualQualityPreset, RenderVisualQualityPreset::Off);
    EXPECT_STREQ(GetRenderVisualQualityPresetName(settings.visualQualityPreset), "off");
    EXPECT_FALSE(settings.enableToneMapping);
    EXPECT_FALSE(settings.enableBloom);
    EXPECT_FALSE(settings.enableFXAA);
    EXPECT_FALSE(settings.enableSSAO);
    EXPECT_FALSE(settings.enableTAA);

    ApplyRenderVisualQualityPreset(settings, RenderVisualQualityPreset::Low);
    EXPECT_EQ(settings.visualQualityPreset, RenderVisualQualityPreset::Low);
    EXPECT_TRUE(settings.enableToneMapping);
    EXPECT_TRUE(settings.enableFXAA);
    EXPECT_FALSE(settings.enableBloom);
    EXPECT_FALSE(settings.enableSSAO);

    ApplyRenderVisualQualityPreset(settings, RenderVisualQualityPreset::Cinematic);
    EXPECT_EQ(settings.visualQualityPreset, RenderVisualQualityPreset::Cinematic);
    EXPECT_TRUE(settings.enableToneMapping);
    EXPECT_TRUE(settings.enableBloom);
    EXPECT_TRUE(settings.enableFXAA);
    EXPECT_TRUE(settings.enableFilmGrain);
    EXPECT_TRUE(settings.enableSSAO);
    EXPECT_TRUE(settings.enableSSR);
    EXPECT_TRUE(settings.enableTAA);
    EXPECT_TRUE(settings.enableDOF);
    EXPECT_TRUE(settings.enableMotionBlur);
    EXPECT_TRUE(settings.enableVolumetricLighting);
}

TEST(RenderPostProcessStackValidation, SceneRendererFrameDiagnosticsExposePostProcessEffectPlans)
{
    const fs::path shaderDir = FindShaderDirectory();
    if (shaderDir.empty())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    const fs::path renderRoot = shaderDir.parent_path();
    const std::string sceneRendererHeader =
        ReadTextFile(renderRoot / "Include" / "Render" / "Renderer" / "SceneRenderer.h");
    const std::string sceneRendererSource =
        ReadTextFile(renderRoot / "Private" / "Renderer" / "SceneRenderer.cpp");

    EXPECT_NE(sceneRendererHeader.find("std::vector<PostProcessEffectExecutionPlan> postProcessEffectPlans;"),
              std::string::npos);
    EXPECT_NE(sceneRendererHeader.find("bool postProcessFallbackCopyApplied = false;"),
              std::string::npos);
    EXPECT_NE(sceneRendererHeader.find("uint32 scheduledPostProcessEffectCount = 0;"),
              std::string::npos);
    EXPECT_NE(sceneRendererHeader.find("RenderVisualQualityPreset requestedVisualQualityPreset = RenderVisualQualityPreset::Medium;"),
              std::string::npos);
    EXPECT_NE(sceneRendererHeader.find("bool postProcessDepthInputAvailable = false;"),
              std::string::npos);
    EXPECT_NE(sceneRendererHeader.find("bool postProcessVelocityInputAvailable = false;"),
              std::string::npos);
    EXPECT_NE(sceneRendererSource.find(
                  "diagnostics.postProcessEffectPlans = m_postProcessStats.stackStats.effectPlans;"),
              std::string::npos);
    EXPECT_NE(sceneRendererSource.find(
                  "diagnostics.postProcessFallbackCopyApplied = m_postProcessStats.stackStats.fallbackCopyApplied;"),
              std::string::npos);
    EXPECT_NE(sceneRendererSource.find(
                  "diagnostics.scheduledPostProcessEffectCount = m_postProcessStats.stackStats.scheduledEffectCount;"),
              std::string::npos);
    EXPECT_NE(sceneRendererSource.find(
                  "diagnostics.requestedVisualQualityPreset = m_postProcessStats.stackStats.requestedQualityPreset;"),
              std::string::npos);
    EXPECT_NE(sceneRendererSource.find(
                  "diagnostics.postProcessDepthInputAvailable = m_postProcessStats.frameInputDepthAvailable;"),
              std::string::npos);
}

TEST(RenderPostProcessStackValidation, SceneRendererFrameDiagnosticsExposeRayTracingSceneStats)
{
    const fs::path shaderDir = FindShaderDirectory();
    if (shaderDir.empty())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    const fs::path renderRoot = shaderDir.parent_path();
    const std::string sceneRendererHeader =
        ReadTextFile(renderRoot / "Include" / "Render" / "Renderer" / "SceneRenderer.h");
    const std::string sceneRendererSource =
        ReadTextFile(renderRoot / "Private" / "Renderer" / "SceneRenderer.cpp");

    EXPECT_NE(sceneRendererHeader.find("RayTracingSceneManagerStats rayTracingSceneStats;"),
              std::string::npos);
    EXPECT_NE(sceneRendererSource.find("diagnostics.rayTracingSceneStats = m_rayTracingSceneStats;"),
              std::string::npos);
}

TEST(RenderPostProcessStackValidation, SceneRendererFrameDiagnosticsExposeGPUResourceStats)
{
    const fs::path shaderDir = FindShaderDirectory();
    if (shaderDir.empty())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    const fs::path renderRoot = shaderDir.parent_path();
    const std::string sceneRendererHeader =
        ReadTextFile(renderRoot / "Include" / "Render" / "Renderer" / "SceneRenderer.h");
    const std::string sceneRendererSource =
        ReadTextFile(renderRoot / "Private" / "Renderer" / "SceneRenderer.cpp");

    EXPECT_NE(sceneRendererHeader.find("RenderResourceRegistryStats gpuResourceStats;"),
              std::string::npos);
    EXPECT_NE(sceneRendererSource.find("diagnostics.gpuResourceStats = m_renderResourceRegistry->GetStats();"),
              std::string::npos);
}

TEST(RenderPostProcessStackValidation, SceneRendererFrameDiagnosticsExposeFeatureExtractionStats)
{
    const fs::path shaderDir = FindShaderDirectory();
    if (shaderDir.empty())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    const fs::path renderRoot = shaderDir.parent_path();
    const std::string sceneRendererHeader =
        ReadTextFile(renderRoot / "Include" / "Render" / "Renderer" / "SceneRenderer.h");
    const std::string sceneRendererSource =
        ReadTextFile(renderRoot / "Private" / "Renderer" / "SceneRenderer.cpp");
    const std::string artifactSource =
        ReadTextFile(renderRoot / "Private" / "Diagnostics" / "RenderToolArtifacts.cpp");
    const std::string renderCMake = ReadTextFile(renderRoot / "CMakeLists.txt");

    EXPECT_NE(sceneRendererHeader.find("struct SceneFeatureExtractionStats"), std::string::npos);
    EXPECT_NE(sceneRendererHeader.find("size_t particleMetadataOnlyCount = 0;"), std::string::npos);
    EXPECT_NE(sceneRendererHeader.find("size_t particleRenderPayloadReadyCount = 0;"), std::string::npos);
    EXPECT_NE(sceneRendererHeader.find("size_t particleSortingSupportedCount = 0;"), std::string::npos);
    EXPECT_NE(sceneRendererHeader.find("RenderFeatureSnapshot m_featureSnapshot;"), std::string::npos);
    EXPECT_NE(sceneRendererHeader.find("ParticleFeaturePass* m_particleFeaturePass = nullptr;"),
              std::string::npos);
    EXPECT_NE(sceneRendererHeader.find("const ParticleFeaturePassStats& GetParticleFeaturePassStats() const;"),
              std::string::npos);
    EXPECT_NE(sceneRendererHeader.find("SceneFeatureExtractionStats m_featureExtractionStats;"),
              std::string::npos);
    EXPECT_NE(sceneRendererHeader.find("SceneFeatureExtractionStats featureExtractionStats;"),
              std::string::npos);
    EXPECT_NE(sceneRendererSource.find("m_featureSnapshot = m_renderScene.GetFeatures();"),
              std::string::npos);
    EXPECT_NE(sceneRendererSource.find("std::make_unique<ParticleFeaturePass>()"), std::string::npos);
    EXPECT_NE(sceneRendererSource.find("m_particleFeaturePass->SetSnapshot(&m_featureSnapshot.particles)"),
              std::string::npos);
    EXPECT_NE(sceneRendererSource.find("return m_particleFeaturePass ? m_particleFeaturePass->GetStats()"),
              std::string::npos);
    EXPECT_NE(sceneRendererSource.find("m_featureSnapshot.GetMetadata()"), std::string::npos);
    EXPECT_NE(sceneRendererSource.find("ParticleRenderSnapshotPayloadStatus::MetadataOnly"), std::string::npos);
    EXPECT_NE(sceneRendererSource.find("ParticleRenderSnapshotPayloadStatus::RenderOwnedPayloadReady"),
              std::string::npos);
    EXPECT_NE(sceneRendererSource.find("diagnostics.featureExtractionStats = m_featureExtractionStats;"),
              std::string::npos);
    EXPECT_NE(sceneRendererSource.find("m_featureExtractionStats.usedProviderPath = true;"),
              std::string::npos);
    EXPECT_NE(sceneRendererSource.find("m_featureExtractionStats.requiresLegacyFallback = false;"),
              std::string::npos);
    EXPECT_NE(artifactSource.find("FeatureExtraction: attempted="), std::string::npos);
    EXPECT_NE(artifactSource.find("\\\"featureExtraction\\\": {"), std::string::npos);
    EXPECT_NE(artifactSource.find("\\\"particleItemCount\\\": "), std::string::npos);
    EXPECT_NE(artifactSource.find("\\\"particleMetadataOnlyCount\\\": "), std::string::npos);
    EXPECT_NE(artifactSource.find("\\\"particleRenderPayloadReadyCount\\\": "), std::string::npos);
    EXPECT_NE(artifactSource.find("\\\"particleSortingSupportedCount\\\": "), std::string::npos);
    EXPECT_NE(artifactSource.find("\\\"waterItemCount\\\": "), std::string::npos);
    EXPECT_NE(artifactSource.find("\\\"terrainItemCount\\\": "), std::string::npos);
    EXPECT_NE(renderCMake.find("Private/Passes/ParticleFeaturePass.cpp"), std::string::npos);
}

TEST(RenderPostProcessStackValidation, PostProcessStackReportsEffectExecutionPlanDomainsAndTargets)
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
    auto* bloom = stack.AddEffect<RecordingPostProcessPass>("Bloom", 100);
    auto* toneMapping = stack.AddEffect<RecordingPostProcessPass>("ToneMapping", 200);
    auto* fxaa = stack.AddEffect<RecordingPostProcessPass>("FXAA", 300);

    stack.Execute(graph, input, output);

    const PostProcessStackExecuteStats& executeStats = stack.GetLastExecuteStats();
    ASSERT_EQ(executeStats.effectPlans.size(), static_cast<size_t>(3));
    EXPECT_EQ(executeStats.enabledEffectCount, 3u);
    EXPECT_EQ(executeStats.scheduledEffectCount, 3u);
    EXPECT_EQ(executeStats.graphPassCount, 3u);
    EXPECT_EQ(executeStats.requestedQualityPreset, RenderVisualQualityPreset::Medium);
    EXPECT_EQ(executeStats.appliedQualityPreset, RenderVisualQualityPreset::Medium);
    EXPECT_EQ(executeStats.transientIntermediateCount, 2u);
    EXPECT_EQ(executeStats.hdrIntermediateCount, 1u);
    EXPECT_EQ(executeStats.ldrIntermediateCount, 1u);

    const PostProcessEffectExecutionPlan& bloomPlan = executeStats.effectPlans[0];
    EXPECT_EQ(bloomPlan.effectName, "Bloom");
    EXPECT_EQ(bloomPlan.sequenceIndex, 0u);
    EXPECT_TRUE(bloomPlan.requested);
    EXPECT_TRUE(bloomPlan.supported);
    EXPECT_TRUE(bloomPlan.enabled);
    EXPECT_TRUE(bloomPlan.scheduled);
    EXPECT_EQ(bloomPlan.inputFormat, RHIFormat::RGBA16_FLOAT);
    EXPECT_EQ(bloomPlan.outputFormat, RHIFormat::RGBA16_FLOAT);
    EXPECT_EQ(bloomPlan.inputDomain, PostProcessColorDomain::HDR);
    EXPECT_EQ(bloomPlan.outputDomain, PostProcessColorDomain::HDR);
    EXPECT_TRUE(bloomPlan.inputIsSceneColor);
    EXPECT_TRUE(bloomPlan.outputIsTransientIntermediate);
    EXPECT_FALSE(bloomPlan.outputIsFinalTarget);
    EXPECT_TRUE(bloomPlan.skippedReason.empty());
    EXPECT_TRUE(bloomPlan.reason.empty());

    const PostProcessEffectExecutionPlan& toneMappingPlan = executeStats.effectPlans[1];
    EXPECT_EQ(toneMappingPlan.effectName, "ToneMapping");
    EXPECT_TRUE(toneMappingPlan.scheduled);
    EXPECT_EQ(toneMappingPlan.inputFormat, RHIFormat::RGBA16_FLOAT);
    EXPECT_EQ(toneMappingPlan.outputFormat, RHIFormat::RGBA8_UNORM);
    EXPECT_EQ(toneMappingPlan.inputDomain, PostProcessColorDomain::HDR);
    EXPECT_EQ(toneMappingPlan.outputDomain, PostProcessColorDomain::LDR);
    EXPECT_FALSE(toneMappingPlan.inputIsSceneColor);
    EXPECT_TRUE(toneMappingPlan.outputIsTransientIntermediate);
    EXPECT_FALSE(toneMappingPlan.outputIsFinalTarget);

    const PostProcessEffectExecutionPlan& fxaaPlan = executeStats.effectPlans[2];
    EXPECT_EQ(fxaaPlan.effectName, "FXAA");
    EXPECT_TRUE(fxaaPlan.scheduled);
    EXPECT_EQ(fxaaPlan.inputFormat, RHIFormat::RGBA8_UNORM);
    EXPECT_EQ(fxaaPlan.outputFormat, RHIFormat::RGBA8_UNORM);
    EXPECT_EQ(fxaaPlan.inputDomain, PostProcessColorDomain::LDR);
    EXPECT_EQ(fxaaPlan.outputDomain, PostProcessColorDomain::LDR);
    EXPECT_FALSE(fxaaPlan.outputIsTransientIntermediate);
    EXPECT_TRUE(fxaaPlan.outputIsFinalTarget);

    EXPECT_EQ(bloom->lastInput.index, input.index);
    EXPECT_EQ(toneMapping->lastInput.index, bloom->lastOutput.index);
    EXPECT_EQ(fxaa->lastInput.index, toneMapping->lastOutput.index);
    EXPECT_EQ(fxaa->lastOutput.index, output.index);

    graph.Compile();
    const auto& graphStats = graph.GetCompileStats();
    EXPECT_TRUE(graphStats.compileValid);
    EXPECT_EQ(graphStats.totalPasses, 3u);
}

TEST_F(RenderPassValidationFixture, PostProcessFrameInputContractReportsMissingVelocityDepthAndHistory)
{
    RenderGraph graph;
    graph.SetDevice(&device);

    RHITextureRef inputTexture =
        device.CreateTexture(RHITextureDesc::RenderTarget(32, 32, RHIFormat::RGBA16_FLOAT));
    RHITextureRef outputTexture =
        device.CreateTexture(RHITextureDesc::RenderTarget(32, 32, RHIFormat::RGBA16_FLOAT));
    ASSERT_TRUE(inputTexture);
    ASSERT_TRUE(outputTexture);

    PostProcessFrameInputs inputs;
    inputs.sceneColor = graph.ImportTexture(inputTexture.Get(), RHIResourceState::ShaderResource);
    inputs.outputFormat = RHIFormat::RGBA16_FLOAT;
    inputs.resetTemporalHistory = true;
    RGTextureHandle output = graph.ImportTexture(outputTexture.Get(), RHIResourceState::RenderTarget);
    graph.SetExportState(output, RHIResourceState::RenderTarget);

    PostProcessStack stack;
    auto* motionBlur = stack.AddEffect<RecordingPostProcessPass>("MotionBlur", 100);
    motionBlur->requirements.requiresDepth = true;
    motionBlur->requirements.requiresVelocity = true;
    motionBlur->requirements.requiresHistory = true;

    stack.Execute(graph, inputs, output);

    const PostProcessStackExecuteStats& executeStats = stack.GetLastExecuteStats();
    EXPECT_TRUE(executeStats.noEffectNoWork);
    EXPECT_EQ(executeStats.enabledEffectCount, 0u);
    ASSERT_EQ(executeStats.effectPlans.size(), static_cast<size_t>(1));

    const PostProcessEffectExecutionPlan& plan = executeStats.effectPlans[0];
    EXPECT_TRUE(plan.requested);
    EXPECT_TRUE(plan.supported);
    EXPECT_TRUE(plan.pipelineReady);
    EXPECT_TRUE(plan.requiresDepth);
    EXPECT_TRUE(plan.requiresVelocity);
    EXPECT_TRUE(plan.requiresHistory);
    EXPECT_FALSE(plan.frameInputsSatisfied);
    EXPECT_NE(plan.missingFrameInputReason.find("depth"), std::string::npos);
    EXPECT_NE(plan.missingFrameInputReason.find("velocity"), std::string::npos);
    EXPECT_NE(plan.missingFrameInputReason.find("temporal history"), std::string::npos);
    EXPECT_EQ(plan.skippedReason, plan.missingFrameInputReason);
    EXPECT_FALSE(plan.scheduled);
    EXPECT_EQ(plan.reason, plan.missingFrameInputReason);
    EXPECT_TRUE(executeStats.fallbackCopyApplied);
}

TEST(RenderPostProcessStackValidation, DOFAndMotionBlurDeclareFrameInputRequirements)
{
    DOFPass dof;
    const PostProcessFrameInputRequirements dofRequirements = dof.GetFrameInputRequirements();
    EXPECT_TRUE(dofRequirements.requiresDepth);
    EXPECT_FALSE(dofRequirements.requiresVelocity);

    MotionBlurPass motionBlur;
    const PostProcessFrameInputRequirements motionRequirements =
        motionBlur.GetFrameInputRequirements();
    EXPECT_TRUE(motionRequirements.requiresDepth);
    EXPECT_TRUE(motionRequirements.requiresVelocity);
    EXPECT_TRUE(motionRequirements.requiresHistory);
}

TEST(RenderPostProcessStackValidation, MotionBlurReportsUnsupportedDiagnosticsWithoutScheduling)
{
    MotionBlurPass motionBlur;
    PostProcessSettings settings;
    settings.enableMotionBlur = true;
    settings.motionBlurIntensity = 0.75f;
    settings.motionBlurMaxVelocity = 24.0f;
    motionBlur.Configure(settings);
    motionBlur.SetCameraMatrices(Mat4Identity(), Mat4Identity());

    const MotionBlurDiagnostics& diagnostics = motionBlur.GetLastDiagnostics();
    EXPECT_TRUE(diagnostics.requested);
    EXPECT_FALSE(diagnostics.supported);
    EXPECT_FALSE(diagnostics.scheduled);
    EXPECT_TRUE(diagnostics.cameraDataAvailable);
    EXPECT_FALSE(diagnostics.velocityAvailable);
    EXPECT_FALSE(diagnostics.depthAvailable);
    EXPECT_TRUE(diagnostics.historyAvailable);
    EXPECT_EQ(diagnostics.sampleCount, 8u);
    EXPECT_EQ(diagnostics.implementationTier, MotionBlurImplementationTier::Unsupported);
    EXPECT_STREQ(GetMotionBlurImplementationTierName(diagnostics.implementationTier), "Unsupported");
    EXPECT_NE(diagnostics.reason.find("velocity gather pipeline"), std::string::npos);

    const fs::path motionBlurSourcePath =
        FindShaderDirectory().parent_path() / "Private" / "PostProcess" / "MotionBlur.cpp";
    const std::string motionBlurSource = ReadTextFile(motionBlurSourcePath);
    EXPECT_EQ(motionBlurSource.find("graph.AddPass"), std::string::npos);
    EXPECT_EQ(motionBlurSource.find("TODO"), std::string::npos);
}

TEST(RenderPostProcessStackValidation, UnsupportedCinematicEffectsReportDiagnosticsWithoutScheduling)
{
    DOFPass dof;
    PostProcessSettings dofSettings;
    dofSettings.enableDOF = true;
    dofSettings.dofFocusDistance = 8.0f;
    dof.Configure(dofSettings);

    const DOFDiagnostics& dofDiagnostics = dof.GetLastDiagnostics();
    EXPECT_TRUE(dofDiagnostics.requested);
    EXPECT_FALSE(dofDiagnostics.supported);
    EXPECT_FALSE(dofDiagnostics.scheduled);
    EXPECT_FALSE(dofDiagnostics.depthAvailable);
    EXPECT_EQ(dofDiagnostics.sampleCount, 8u);
    EXPECT_EQ(dofDiagnostics.implementationTier, DOFImplementationTier::Unsupported);
    EXPECT_STREQ(GetDOFImplementationTierName(dofDiagnostics.implementationTier), "Unsupported");
    EXPECT_NE(dofDiagnostics.reason.find("gather/composite pipeline"), std::string::npos);

    VolumetricLightingPass volumetric;
    PostProcessSettings volumetricSettings;
    volumetricSettings.enableVolumetricLighting = true;
    volumetric.Configure(volumetricSettings);

    const VolumetricLightingDiagnostics& volumetricDiagnostics =
        volumetric.GetLastDiagnostics();
    EXPECT_TRUE(volumetricDiagnostics.requested);
    EXPECT_FALSE(volumetricDiagnostics.supported);
    EXPECT_FALSE(volumetricDiagnostics.scheduled);
    EXPECT_FALSE(volumetricDiagnostics.depthAvailable);
    EXPECT_FALSE(volumetricDiagnostics.shadowMapAvailable);
    EXPECT_TRUE(volumetricDiagnostics.temporalRequested);
    EXPECT_TRUE(volumetricDiagnostics.halfResolution);
    EXPECT_EQ(volumetricDiagnostics.sampleCount, 32u);
    EXPECT_EQ(volumetricDiagnostics.implementationTier,
              VolumetricLightingImplementationTier::Unsupported);
    EXPECT_STREQ(GetVolumetricLightingImplementationTierName(
                     volumetricDiagnostics.implementationTier),
                 "Unsupported");
    EXPECT_NE(volumetricDiagnostics.reason.find("ray march/composite pipeline"),
              std::string::npos);

    const fs::path postProcessDir =
        FindShaderDirectory().parent_path() / "Private" / "PostProcess";
    const std::string dofSource = ReadTextFile(postProcessDir / "DOF.cpp");
    const std::string volumetricSource =
        ReadTextFile(postProcessDir / "VolumetricLighting.cpp");
    EXPECT_EQ(dofSource.find("graph.AddPass"), std::string::npos);
    EXPECT_EQ(dofSource.find("TODO"), std::string::npos);
    EXPECT_EQ(volumetricSource.find("graph.AddPass"), std::string::npos);
    EXPECT_EQ(volumetricSource.find("TODO"), std::string::npos);
}

TEST_F(RenderPassValidationFixture, SwapChainManagerReportsUnsupportedWindowHandleCreation)
{
    FakeDevice localDevice;
    SwapChainManager manager;
    int fakeWindow = 1;

    manager.Initialize(&localDevice, &fakeWindow, 1280, 720);

    const SwapChainManagerDiagnostics& diagnostics = manager.GetLastDiagnostics();
    EXPECT_TRUE(diagnostics.requested);
    EXPECT_FALSE(diagnostics.initialized);
    EXPECT_TRUE(diagnostics.hasDevice);
    EXPECT_TRUE(diagnostics.hasWindowHandle);
    EXPECT_FALSE(diagnostics.hasSwapChain);
    EXPECT_FALSE(diagnostics.ownsSwapChain);
    EXPECT_EQ(diagnostics.width, 1280u);
    EXPECT_EQ(diagnostics.height, 720u);
    EXPECT_EQ(manager.GetSwapChain(), nullptr);
    EXPECT_FALSE(manager.OwnsSwapChain());
    EXPECT_NE(diagnostics.reason.find("provide a platform-created RHISwapChain"), std::string::npos);

    const fs::path swapChainSourcePath =
        FindShaderDirectory().parent_path() / "Private" / "SwapChainManager.cpp";
    const std::string swapChainSource = ReadTextFile(swapChainSourcePath);
    EXPECT_EQ(swapChainSource.find("TODO"), std::string::npos);
}

TEST_F(RenderPassValidationFixture, DebugRendererFrustumQueuesLinesButRenderReportsMissingPipeline)
{
    RecordingCommandContext ctx;
    FakeDevice localDevice;
    localDevice.EnableBasicCapabilities();

    DebugRenderer debugRenderer;
    debugRenderer.Initialize(&localDevice, nullptr);
    ASSERT_TRUE(debugRenderer.IsInitialized());

    debugRenderer.BeginFrame();
    debugRenderer.DrawFrustum(Mat4Identity(), Vec4(1.0f, 0.8f, 0.2f, 1.0f));

    ViewData debugView;
    debugRenderer.Render(ctx, debugView);

    const DebugRendererDiagnostics& diagnostics = debugRenderer.GetLastDiagnostics();
    EXPECT_TRUE(diagnostics.requested);
    EXPECT_FALSE(diagnostics.supported);
    EXPECT_FALSE(diagnostics.scheduled);
    EXPECT_FALSE(diagnostics.executed);
    EXPECT_TRUE(diagnostics.initialized);
    EXPECT_TRUE(diagnostics.enabled);
    EXPECT_TRUE(diagnostics.vertexBufferAvailable);
    EXPECT_FALSE(diagnostics.pipelineAvailable);
    EXPECT_TRUE(diagnostics.depthTestEnabled);
    EXPECT_EQ(diagnostics.vertexCount, 24u);
    EXPECT_NE(diagnostics.reason.find("Debug line pipeline"), std::string::npos);

    const fs::path debugSourcePath =
        FindShaderDirectory().parent_path() / "Private" / "Debug" / "DebugRenderer.cpp";
    const std::string debugSource = ReadTextFile(debugSourcePath);
    EXPECT_EQ(debugSource.find("TODO"), std::string::npos);
}

TEST_F(RenderPassValidationFixture, DecalRendererReportsUnsupportedWithoutSchedulingGraphPass)
{
    FakeDevice localDevice;
    localDevice.EnableBasicCapabilities();

    DecalRenderer decals;
    decals.Initialize(&localDevice);
    ASSERT_TRUE(decals.IsInitialized());
    EXPECT_FALSE(decals.IsSupported());
    EXPECT_FALSE(decals.GetUnsupportedReason().empty());

    DecalData decal;
    decal.transform = Mat4Identity();
    EXPECT_NE(decals.AddDecal(decal), RVX_INVALID_INDEX);

    RenderGraph graph;
    graph.SetDevice(&localDevice);

    RHITextureRef albedoTexture =
        localDevice.CreateTexture(RHITextureDesc::RenderTarget(64, 64, RHIFormat::RGBA8_UNORM));
    RHITextureRef normalTexture =
        localDevice.CreateTexture(RHITextureDesc::RenderTarget(64, 64, RHIFormat::RGBA16_FLOAT));
    RHITextureRef roughnessTexture =
        localDevice.CreateTexture(RHITextureDesc::RenderTarget(64, 64, RHIFormat::R8_UNORM));
    RHITextureRef depthTexture =
        localDevice.CreateTexture(RHITextureDesc::DepthStencil(64, 64, RHIFormat::D32_FLOAT));
    ASSERT_TRUE(albedoTexture);
    ASSERT_TRUE(normalTexture);
    ASSERT_TRUE(roughnessTexture);
    ASSERT_TRUE(depthTexture);

    RGTextureHandle albedo = graph.ImportTexture(albedoTexture.Get(), RHIResourceState::RenderTarget);
    RGTextureHandle normal = graph.ImportTexture(normalTexture.Get(), RHIResourceState::RenderTarget);
    RGTextureHandle roughness = graph.ImportTexture(roughnessTexture.Get(), RHIResourceState::RenderTarget);
    RGTextureHandle depth = graph.ImportTexture(depthTexture.Get(), RHIResourceState::DepthRead);

    decals.AddToGraph(graph, albedo, normal, roughness, depth, Mat4Identity(), Mat4Identity());

    const DecalRendererDiagnostics& diagnostics = decals.GetLastDiagnostics();
    EXPECT_TRUE(diagnostics.requested);
    EXPECT_FALSE(diagnostics.supported);
    EXPECT_FALSE(diagnostics.scheduled);
    EXPECT_FALSE(diagnostics.executed);
    EXPECT_TRUE(diagnostics.initialized);
    EXPECT_TRUE(diagnostics.enabled);
    EXPECT_TRUE(diagnostics.gBufferAlbedoAvailable);
    EXPECT_TRUE(diagnostics.gBufferNormalAvailable);
    EXPECT_TRUE(diagnostics.gBufferRoughnessAvailable);
    EXPECT_TRUE(diagnostics.depthAvailable);
    EXPECT_EQ(diagnostics.decalCount, 1u);
    EXPECT_EQ(diagnostics.implementationTier, DecalRendererImplementationTier::Unsupported);
    EXPECT_STREQ(GetDecalRendererImplementationTierName(diagnostics.implementationTier), "Unsupported");
    EXPECT_NE(diagnostics.reason.find("not implemented"), std::string::npos);

    graph.Compile();
    EXPECT_EQ(graph.GetCompileStats().totalPasses, 0u);

    const fs::path decalSourcePath =
        FindShaderDirectory().parent_path() / "Private" / "Decal" / "DecalRenderer.cpp";
    const std::string decalSource = ReadTextFile(decalSourcePath);
    EXPECT_EQ(decalSource.find("graph.AddPass"), std::string::npos);
    EXPECT_EQ(decalSource.find("TODO"), std::string::npos);
}

TEST_F(RenderPassValidationFixture, SSRReportsUnsupportedDiagnosticsWithoutScheduling)
{
    RecordingCommandContext ctx;
    FakeDevice localDevice;
    localDevice.EnableBasicCapabilities();

    SSR ssr;
    ssr.SetEnabled(true);
    ssr.Initialize(&localDevice, 64, 48);

    ASSERT_TRUE(ssr.IsInitialized());
    EXPECT_FALSE(ssr.IsSupported());
    EXPECT_FALSE(ssr.IsEnabled());
    EXPECT_FALSE(ssr.GetUnsupportedReason().empty());

    RHITextureRef ssrColorTexture =
        localDevice.CreateTexture(RHITextureDesc::RenderTarget(64, 48, RHIFormat::RGBA16_FLOAT));
    RHITextureRef ssrDepthTexture =
        localDevice.CreateTexture(RHITextureDesc::DepthStencil(64, 48, RHIFormat::D32_FLOAT));
    RHITextureRef ssrNormalTexture =
        localDevice.CreateTexture(RHITextureDesc::RenderTarget(64, 48, RHIFormat::RGBA16_FLOAT));
    RHITextureRef ssrRoughnessTexture =
        localDevice.CreateTexture(RHITextureDesc::RenderTarget(64, 48, RHIFormat::R8_UNORM));
    ASSERT_TRUE(ssrColorTexture);
    ASSERT_TRUE(ssrDepthTexture);
    ASSERT_TRUE(ssrNormalTexture);
    ASSERT_TRUE(ssrRoughnessTexture);

    ssr.Compute(ctx,
                ssrColorTexture.Get(),
                ssrDepthTexture.Get(),
                ssrNormalTexture.Get(),
                ssrRoughnessTexture.Get(),
                Mat4Identity(),
                Mat4Identity());

    const SSRComputeStats& diagnostics = ssr.GetLastComputeStats();
    EXPECT_TRUE(diagnostics.requested);
    EXPECT_FALSE(diagnostics.supported);
    EXPECT_FALSE(diagnostics.scheduled);
    EXPECT_FALSE(diagnostics.executed);
    EXPECT_TRUE(diagnostics.colorAvailable);
    EXPECT_TRUE(diagnostics.depthAvailable);
    EXPECT_TRUE(diagnostics.normalAvailable);
    EXPECT_TRUE(diagnostics.roughnessAvailable);
    EXPECT_TRUE(diagnostics.temporalHistoryRequired);
    EXPECT_EQ(diagnostics.implementationTier, SSRImplementationTier::Unsupported);
    EXPECT_STREQ(GetSSRImplementationTierName(diagnostics.implementationTier), "Unsupported");
    EXPECT_TRUE(diagnostics.missingInputReason.empty());
    EXPECT_NE(diagnostics.fallbackReason.find("not implemented"), std::string::npos);

    const fs::path ssrSourcePath =
        FindShaderDirectory().parent_path() / "Private" / "PostProcess" / "SSR.cpp";
    const std::string ssrSource = ReadTextFile(ssrSourcePath);
    EXPECT_EQ(ssrSource.find("graph.AddPass"), std::string::npos);
    EXPECT_EQ(ssrSource.find("TODO"), std::string::npos);
}

TEST_F(RenderPassValidationFixture, TAAMinimalResolveCopiesCurrentFrameAndUpdatesHistory)
{
    FakeDevice localDevice;
    RecordingCommandContext ctx;

    TAA taa;
    taa.Initialize(&localDevice, 32, 24);
    ASSERT_TRUE(taa.IsInitialized());
    EXPECT_TRUE(taa.IsSupported());
    EXPECT_TRUE(taa.IsEnabled());
    EXPECT_FALSE(taa.HasValidHistory());

    RHITextureRef currentColor =
        localDevice.CreateTexture(RHITextureDesc::RenderTarget(32, 24, RHIFormat::RGBA16_FLOAT));
    ASSERT_TRUE(currentColor);

    taa.Resolve(ctx, currentColor.Get(), nullptr, nullptr, 17);

    const TAAResolveStats& stats = taa.GetLastResolveStats();
    EXPECT_TRUE(stats.requested);
    EXPECT_TRUE(stats.supported);
    EXPECT_TRUE(stats.resolved);
    EXPECT_TRUE(stats.copiedCurrentFrame);
    EXPECT_FALSE(stats.historyValidBefore);
    EXPECT_TRUE(stats.historyValidAfter);
    EXPECT_FALSE(stats.depthAvailable);
    EXPECT_FALSE(stats.motionVectorsAvailable);
    EXPECT_TRUE(stats.motionVectorFallbackUsed);
    EXPECT_EQ(stats.frameIndex, 17u);
    EXPECT_EQ(ctx.copyTextureCount, 2u);
    EXPECT_TRUE(taa.HasValidHistory());
}

TEST_F(RenderPassValidationFixture, StandaloneSSAOAndSSRReportMissingFrameInputs)
{
    RecordingCommandContext ctx;

    SSAO ssao;
    ssao.SetEnabled(true);
    ssao.Compute(ctx, nullptr, nullptr, Mat4Identity(), Mat4Identity());
    const SSAOComputeStats& ssaoStats = ssao.GetLastComputeStats();
    EXPECT_TRUE(ssaoStats.requested);
    EXPECT_FALSE(ssaoStats.supported);
    EXPECT_FALSE(ssaoStats.executed);
    EXPECT_FALSE(ssaoStats.depthAvailable);
    EXPECT_NE(ssaoStats.fallbackReason.find("depth"), std::string::npos);

    SSR ssr;
    ssr.SetEnabled(true);
    ssr.Compute(ctx, nullptr, nullptr, nullptr, nullptr, Mat4Identity(), Mat4Identity());
    const SSRComputeStats& ssrStats = ssr.GetLastComputeStats();
    EXPECT_TRUE(ssrStats.requested);
    EXPECT_FALSE(ssrStats.supported);
    EXPECT_FALSE(ssrStats.scheduled);
    EXPECT_FALSE(ssrStats.executed);
    EXPECT_FALSE(ssrStats.colorAvailable);
    EXPECT_FALSE(ssrStats.depthAvailable);
    EXPECT_FALSE(ssrStats.normalAvailable);
    EXPECT_FALSE(ssrStats.roughnessAvailable);
    EXPECT_EQ(ssrStats.implementationTier, SSRImplementationTier::Unsupported);
    EXPECT_NE(ssrStats.missingInputReason.find("color, depth, normal, or roughness"),
              std::string::npos);
    EXPECT_EQ(ssrStats.fallbackReason, ssrStats.missingInputReason);
}

TEST_F(RenderPassValidationFixture, StandaloneSSAOInitializesMinimalLowTierAndReportsFallbacks)
{
    RecordingCommandContext ctx;
    FakeDevice localDevice;
    localDevice.EnableBasicCapabilities();

    SSAOConfig config;
    config.quality = SSAOQuality::High;
    config.temporalFilter = true;
    config.blurPasses = 2;

    SSAO ssao;
    ssao.SetConfig(config);
    ssao.SetEnabled(true);
    ssao.Initialize(&localDevice, 64, 48);

    EXPECT_TRUE(ssao.IsInitialized());
    EXPECT_TRUE(ssao.IsSupported());
    EXPECT_TRUE(ssao.IsEnabled());
    EXPECT_TRUE(ssao.GetUnsupportedReason().empty());
    ASSERT_NE(ssao.GetResult(), nullptr);
    ASSERT_NE(ssao.GetBlurredResult(), nullptr);

    RHITextureRef depthTexture =
        localDevice.CreateTexture(RHITextureDesc::DepthStencil(64, 48, RHIFormat::D32_FLOAT));
    ASSERT_TRUE(depthTexture);

    ssao.Compute(ctx, depthTexture.Get(), nullptr, Mat4Identity(), Mat4Identity());

    const SSAOComputeStats& stats = ssao.GetLastComputeStats();
    EXPECT_TRUE(stats.requested);
    EXPECT_TRUE(stats.supported);
    EXPECT_TRUE(stats.executed);
    EXPECT_TRUE(stats.depthAvailable);
    EXPECT_FALSE(stats.normalAvailable);
    EXPECT_TRUE(stats.normalFallbackUsed);
    EXPECT_TRUE(stats.temporalFallbackUsed);
    EXPECT_TRUE(stats.neutralOutputFallbackUsed);
    EXPECT_EQ(stats.sampleCount, 16u);
    EXPECT_EQ(stats.aoPassCount, 1u);
    EXPECT_EQ(stats.blurPassCount, 1u);
    EXPECT_EQ(stats.implementationTier, SSAOImplementationTier::MinimalNeutralOutput);
    EXPECT_STREQ(GetSSAOImplementationTierName(stats.implementationTier), "MinimalNeutralOutput");
    EXPECT_NE(stats.fallbackReason.find("normal"), std::string::npos);
    EXPECT_NE(stats.fallbackReason.find("temporal"), std::string::npos);
    EXPECT_NE(stats.fallbackReason.find("neutral AO"), std::string::npos);

    EXPECT_EQ(ctx.beginRenderPassCount, 1u);
    EXPECT_EQ(ctx.endRenderPassCount, 1u);
    EXPECT_EQ(ctx.copyTextureCount, 1u);
    ASSERT_EQ(ctx.renderPasses.size(), 1u);
    EXPECT_EQ(ctx.renderPasses[0].colorAttachmentCount, 1u);
    EXPECT_FLOAT_EQ(ctx.renderPasses[0].colorAttachments[0].clearColor.r, 1.0f);
}

TEST_F(RenderPassValidationFixture,
       DepthPrepassB2bOwnsSealedSnapshotsAttachmentsAndSubmissionLifetime)
{
    RVX_REQUIRE_RENDER_RUNTIME_PIPELINE();
    device.SetFenceAutoComplete(false);

    const MeshGPUBuffers buffers =
        gpuResources.GetMeshBuffers(meshResource->GetId());
    ASSERT_TRUE(buffers.IsValid());
    ASSERT_FALSE(buffers.submeshes.empty());

    RenderScene sceneA;
    RenderScene sceneB;
    RenderObject objectA = scene.GetObject(0);
    objectA.entityId = 1801;
    RenderObject objectB = scene.GetObject(0);
    objectB.entityId = 1802;
    sceneA.AddObject(objectA);
    sceneB.AddObject(objectB);

    RenderDrawItem itemA = MakeDrawItem(MaterialRenderMode::Opaque);
    itemA.packet = MakeDepthPacket(
        sceneA, 0, 0, buffers, itemA.material,
        RenderMaterialMode::Opaque, RenderDrawFlags::None);
    RenderDrawItem itemB = MakeDrawItem(MaterialRenderMode::Opaque);
    itemB.packet = MakeDepthPacket(
        sceneB, 0, 0, buffers, itemB.material,
        RenderMaterialMode::Opaque, RenderDrawFlags::None);
    std::vector<RenderDrawItem> opaqueItemsA = {itemA};
    std::vector<RenderDrawItem> opaqueItemsB = {itemB};
    std::vector<RenderDrawItem> maskedItemsA;
    std::vector<RenderDrawItem> maskedItemsB;
    SceneMeshPassPreparation preparationA = PrepareDepthPackets(
        opaqueItemsA, maskedItemsA);
    SceneMeshPassPreparation preparationB = PrepareDepthPackets(
        opaqueItemsB, maskedItemsB);
    RenderFramePlanCompileResult compiledA = CompileForcedDirectPlan(
        preparationA, 1801);
    RenderFramePlanCompileResult compiledB = CompileForcedDirectPlan(
        preparationB, 1802);
    ASSERT_TRUE(compiledA.succeeded);
    ASSERT_TRUE(compiledB.succeeded);
    RenderFrameExecutionReport reportA = MakeExecutionReport(compiledA.plan);
    RenderFrameExecutionReport reportB = MakeExecutionReport(compiledB.plan);

    const auto makeDirectVisibility = [](RenderPassKind pass)
    {
        RenderVisibilityResult visibility;
        RenderVisibilityPassResult& passVisibility =
            visibility.passes[static_cast<size_t>(pass)];
        passVisibility.pass = pass;
        passVisibility.sourcePacketKnown = {1u};
        passVisibility.cpuVisibleBySourcePacket = {1u};
        return visibility;
    };
    RenderVisibilityResult visibilityA = makeDirectVisibility(RenderPassKind::Depth);
    RenderVisibilityResult visibilityB = makeDirectVisibility(RenderPassKind::Depth);

    struct DepthTarget
    {
        RHITextureRef texture;
        uint32 extent = 0;
    };
    const auto makeContext = [this](RenderGraph& graph,
                                    ViewData& callerView,
                                    RenderScene& callerScene,
                                    const std::vector<RenderDrawItem>& opaqueItems,
                                    const std::vector<RenderDrawItem>& maskedItems,
                                    const RenderFrameExecutionPlan& plan,
                                    const SceneMeshPassPreparation& preparation,
                                    RenderFrameExecutionReport& report,
                                    RenderVisibilityResult& visibility,
                                    RenderSubmissionResourceBatch& batch,
                                    DepthTarget& target,
                                    uint64 recordEpoch,
                                    uint32 extent)
    {
        target.extent = extent;
        target.texture = device.CreateTexture(RHITextureDesc::DepthStencil(
            extent, extent, PipelineCache::GetDefaultDepthStencilFormat()));
        EXPECT_TRUE(target.texture);

        callerView = {};
        callerView.renderGraph = &graph;
        callerView.viewCache = &viewCache;
        callerView.submissionResourceBatch = &batch;
        callerView.depthTarget = graph.ImportTexture(
            target.texture.Get(), RHIResourceState::DepthWrite);
        callerView.viewportWidth = extent;
        callerView.viewportHeight = extent;
        callerView.renderVisibility = &visibility;
        graph.SetExportState(callerView.depthTarget, RHIResourceState::DepthRead);
        return MakeMainSceneRecordContext(
            graph, callerView, callerScene, opaqueItems, maskedItems, plan,
            preparation, report, recordEpoch, &batch);
    };

    RenderGraph graphA;
    RenderGraph graphB;
    graphA.SetDevice(&device);
    graphB.SetDevice(&device);
    RenderSubmissionTracker tracker;
    ASSERT_TRUE(tracker.Initialize(&device));
    RenderRetirementQueue retirement;
    ASSERT_TRUE(retirement.Initialize(&tracker));
    RenderSubmissionResourceBatch batchA;
    RenderSubmissionResourceBatch batchB;
    DepthTarget targetA;
    DepthTarget targetB;
    ViewData callerViewA;
    ViewData callerViewB;
    RenderPassRecordContext contextA = makeContext(
        graphA, callerViewA, sceneA, opaqueItemsA, maskedItemsA,
        compiledA.plan, preparationA, reportA, visibilityA, batchA, targetA,
        1801, 48);
    RenderPassRecordContext contextB = makeContext(
        graphB, callerViewB, sceneB, opaqueItemsB, maskedItemsB,
        compiledB.plan, preparationB, reportB, visibilityB, batchB, targetB,
        1802, 96);

    DepthPrepass pass;
    ConfigureResources(pass, gpuResources, pipelineCache);
    pass.SetMaterialSystem(&materialSystem);
    pass.SetEnabled(true);
    pass.AddToGraph(graphA, contextA);
    pass.AddToGraph(graphB, contextB);

    // The callbacks may only observe their per-record snapshots.
    sceneA.Clear();
    sceneB.Clear();
    opaqueItemsA.clear();
    opaqueItemsB.clear();
    maskedItemsA.clear();
    maskedItemsB.clear();
    preparationA.depth.Clear();
    preparationB.depth.Clear();
    compiledA.plan.packetReferences.clear();
    compiledB.plan.packetReferences.clear();
    callerViewA.depthTarget = {};
    callerViewA.viewportWidth = 1;
    callerViewB.depthTarget = {};
    callerViewB.viewportWidth = 1;

    graphB.Compile();
    graphA.Compile();
    ASSERT_TRUE(graphA.GetCompileStats().compileValid);
    ASSERT_TRUE(graphB.GetCompileStats().compileValid);

    RecordingCommandContext commandsB;
    graphB.Execute(commandsB);
    ASSERT_EQ(1u, commandsB.beginRenderPassCount);
    ASSERT_EQ(1u, commandsB.drawIndexedCount);
    ASSERT_EQ(1u, commandsB.renderPasses.size());
    ASSERT_TRUE(commandsB.renderPasses[0].hasDepthStencil);
    ASSERT_NE(nullptr, commandsB.renderPasses[0].depthStencilAttachment.view);
    EXPECT_EQ(targetB.texture.Get(),
              commandsB.renderPasses[0].depthStencilAttachment.view->GetTexture());
    EXPECT_EQ(targetB.extent,
              commandsB.renderPasses[0].depthStencilAttachment.view->GetTexture()->GetWidth());
    EXPECT_EQ(1u, contextB.results->depthStats.directDrawCount);

    RecordingCommandContext commandsA;
    graphA.Execute(commandsA);
    ASSERT_EQ(1u, commandsA.beginRenderPassCount);
    ASSERT_EQ(1u, commandsA.drawIndexedCount);
    ASSERT_EQ(1u, commandsA.renderPasses.size());
    ASSERT_TRUE(commandsA.renderPasses[0].hasDepthStencil);
    ASSERT_NE(nullptr, commandsA.renderPasses[0].depthStencilAttachment.view);
    EXPECT_EQ(targetA.texture.Get(),
              commandsA.renderPasses[0].depthStencilAttachment.view->GetTexture());
    EXPECT_EQ(targetA.extent,
              commandsA.renderPasses[0].depthStencilAttachment.view->GetTexture()->GetWidth());
    EXPECT_EQ(1u, contextA.results->depthStats.directDrawCount);

    RHITextureViewRef depthViewProbeA(
        commandsA.renderPasses[0].depthStencilAttachment.view);
    RHITextureViewRef depthViewProbeB(
        commandsB.renderPasses[0].depthStencilAttachment.view);
    ASSERT_TRUE(depthViewProbeA);
    ASSERT_TRUE(depthViewProbeB);
    RHITextureRef depthTextureProbeA(depthViewProbeA->GetTexture());
    RHITextureRef depthTextureProbeB(depthViewProbeB->GetTexture());
    ASSERT_TRUE(depthTextureProbeA);
    ASSERT_TRUE(depthTextureProbeB);

    GPUCompletionToken completionB;
    GPUCompletionToken completionA;
    ASSERT_TRUE(InsertGPUCompletionPoint(completionB, tracker.Submit(&commandsB)));
    ASSERT_TRUE(InsertGPUCompletionPoint(completionA, tracker.Submit(&commandsA)));
    graphA.Clear();
    graphB.Clear();
    viewCache.Clear();
    targetA.texture.Reset();
    targetB.texture.Reset();
    EXPECT_EQ(2u, depthViewProbeA->GetRefCount());
    EXPECT_EQ(2u, depthViewProbeB->GetRefCount());
    EXPECT_EQ(2u, depthTextureProbeA->GetRefCount());
    EXPECT_EQ(2u, depthTextureProbeB->GetRefCount());

    batchB.SealAndTransfer(completionB, retirement);
    batchA.SealAndTransfer(completionA, retirement);
    FakeFence* const graphicsFence =
        device.FindFenceWithSignal(completionA.points[0].value);
    ASSERT_NE(nullptr, graphicsFence);
    graphicsFence->Complete(completionB.points[0].value);
    EXPECT_EQ(GPUCompletionStatus::Pending, retirement.Poll());
    EXPECT_EQ(2u, depthViewProbeA->GetRefCount());
    EXPECT_EQ(1u, depthViewProbeB->GetRefCount());
    EXPECT_EQ(2u, depthTextureProbeA->GetRefCount());
    EXPECT_EQ(1u, depthTextureProbeB->GetRefCount());
    graphicsFence->Complete(completionA.points[0].value);
    EXPECT_EQ(GPUCompletionStatus::Completed, retirement.Poll());
    EXPECT_EQ(1u, depthViewProbeA->GetRefCount());
    EXPECT_EQ(1u, depthTextureProbeA->GetRefCount());
    tracker.Shutdown();

    // Clear advances the generation. The stale context must neither declare a
    // depth usage nor own or mutate the caller's results sink.
    contextA.results->depthStats.directDrawCount = 71;
    contextA.results->depthStats.failureReason = RenderPolicyReason::None;
    contextA.results->executionReport.status = RenderExecutionStatus::Completed;
    const RenderFrameExecutionReport staleReportBefore =
        contextA.results->executionReport;
    pass.AddToGraph(graphA, contextA);
    graphA.Compile();
    ASSERT_TRUE(graphA.GetCompileStats().compileValid);
    const RenderGraph::Diagnostics staleDiagnostics = graphA.GetDiagnostics();
    ASSERT_EQ(1u, staleDiagnostics.passes.size());
    EXPECT_TRUE(staleDiagnostics.passes[0].usages.empty());
    RecordingCommandContext staleCommands;
    graphA.Execute(staleCommands);
    EXPECT_EQ(0u, staleCommands.beginRenderPassCount);
    EXPECT_EQ(71u, contextA.results->depthStats.directDrawCount);
    EXPECT_EQ(RenderPolicyReason::None,
              contextA.results->depthStats.failureReason);
    EXPECT_EQ(staleReportBefore.status, contextA.results->executionReport.status);
    EXPECT_EQ(staleReportBefore.frameSequence,
              contextA.results->executionReport.frameSequence);
}

TEST_F(RenderPassValidationFixture,
       OpaquePassB2bOwnsSealedCallerInputsForTypedDirectRecording)
{
    RVX_REQUIRE_RENDER_RUNTIME_PIPELINE();

    const MeshGPUBuffers buffers =
        gpuResources.GetMeshBuffers(meshResource->GetId());
    ASSERT_TRUE(buffers.IsValid());
    ASSERT_FALSE(buffers.submeshes.empty());

    RenderScene callerScene;
    RenderObject callerObject = scene.GetObject(0);
    callerObject.entityId = 1811;
    callerScene.AddObject(callerObject);
    RenderDrawItem opaqueItem = MakeDrawItem(MaterialRenderMode::Opaque);
    opaqueItem.packet = MakeOpaquePacket(
        callerScene, 0, 0, buffers, opaqueItem.material,
        RenderMaterialMode::Opaque, RenderDrawFlags::None);
    std::vector<RenderDrawItem> opaqueItems = {opaqueItem};
    std::vector<RenderDrawItem> maskedItems;
    const SceneMeshPassPreparation preparation = PrepareOpaquePackets(
        opaqueItems, maskedItems);
    const RenderFramePlanCompileResult compiled = CompileForcedDirectPlan(
        preparation, 1811);
    ASSERT_TRUE(compiled.succeeded);
    RenderFrameExecutionReport report = MakeExecutionReport(compiled.plan);
    RenderVisibilityResult visibility;
    RenderVisibilityPassResult& opaqueVisibility =
        visibility.passes[static_cast<size_t>(RenderPassKind::Opaque)];
    opaqueVisibility.pass = RenderPassKind::Opaque;
    opaqueVisibility.sourcePacketKnown = {1u};
    opaqueVisibility.cpuVisibleBySourcePacket = {1u};

    RenderGraph graph;
    graph.SetDevice(&device);
    RHITextureRef colorTarget = device.CreateTexture(
        RHITextureDesc::RenderTarget(80, 48, RHIFormat::RGBA8_UNORM));
    RHITextureRef depthTarget = device.CreateTexture(RHITextureDesc::DepthStencil(
        80, 48, PipelineCache::GetDefaultDepthStencilFormat()));
    ASSERT_TRUE(colorTarget);
    ASSERT_TRUE(depthTarget);
    ViewData callerView;
    callerView.renderGraph = &graph;
    callerView.viewCache = &viewCache;
    callerView.colorTarget = graph.ImportTexture(
        colorTarget.Get(), RHIResourceState::RenderTarget);
    callerView.depthTarget = graph.ImportTexture(
        depthTarget.Get(), RHIResourceState::DepthWrite);
    callerView.viewportWidth = 80;
    callerView.viewportHeight = 48;
    callerView.renderVisibility = &visibility;
    graph.SetExportState(callerView.colorTarget, RHIResourceState::RenderTarget);

    OpaquePass pass;
    ConfigureResources(pass, gpuResources, pipelineCache, materialSystem);
    RenderPassRecordContext context = MakeMainSceneRecordContext(
        graph, callerView, callerScene, opaqueItems, maskedItems,
        compiled.plan, preparation, report, 1811);
    pass.AddToGraph(graph, context);

    callerScene.Clear();
    opaqueItems.clear();
    maskedItems.clear();
    callerView.colorTarget = {};
    callerView.depthTarget = {};
    callerView.viewportWidth = 1;
    callerView.viewportHeight = 1;
    callerView.renderVisibility = nullptr;

    graph.Compile();
    ASSERT_TRUE(graph.GetCompileStats().compileValid);
    RecordingCommandContext commands;
    graph.Execute(commands);
    pass.PublishRecordResults(context.results, context.identity);

    ASSERT_EQ(1u, commands.beginRenderPassCount);
    ASSERT_EQ(1u, commands.endRenderPassCount);
    ASSERT_EQ(1u, commands.drawIndexedCount);
    ASSERT_EQ(1u, commands.renderPasses.size());
    ASSERT_EQ(1u, commands.renderPasses[0].colorAttachmentCount);
    ASSERT_NE(nullptr, commands.renderPasses[0].colorAttachments[0].view);
    EXPECT_EQ(colorTarget.Get(),
              commands.renderPasses[0].colorAttachments[0].view->GetTexture());
    ASSERT_TRUE(commands.renderPasses[0].hasDepthStencil);
    ASSERT_NE(nullptr, commands.renderPasses[0].depthStencilAttachment.view);
    EXPECT_EQ(depthTarget.Get(),
              commands.renderPasses[0].depthStencilAttachment.view->GetTexture());
    EXPECT_EQ(1u, context.results->opaqueStats.plannedPacketCount);
    EXPECT_EQ(1u, context.results->opaqueStats.executedPacketCount);
    EXPECT_EQ(1u, context.results->opaqueStats.directDrawCount);
    EXPECT_EQ(RenderPolicyReason::None,
              context.results->opaqueStats.failureReason);
}

TEST_F(RenderPassValidationFixture,
       ShadowPassB2bEmptyRecordPublishesCurrentCascadeOutputWithoutDraws)
{
    RVX_REQUIRE_RENDER_RUNTIME_PIPELINE();

    RenderScene emptyScene;
    const std::vector<RenderDrawItem> emptyDrawItems;
    const SceneMeshPassPreparation preparation = PrepareOpaquePackets(
        emptyDrawItems, emptyDrawItems);
    const RenderFramePlanCompileResult compiled = CompileForcedDirectPlan(
        preparation, 1821);
    ASSERT_TRUE(compiled.succeeded);
    RenderFrameExecutionReport report = MakeExecutionReport(compiled.plan);

    RenderGraph graph;
    graph.SetDevice(&device);
    ViewData recordView;
    recordView.renderGraph = &graph;
    recordView.viewCache = &viewCache;
    recordView.viewportWidth = 96;
    recordView.viewportHeight = 64;
    recordView.aspectRatio = 1.5f;
    recordView.fieldOfView = 1.0472f;
    recordView.nearPlane = 0.1f;
    recordView.farPlane = 100.0f;
    recordView.cameraPosition = Vec3(0.0f, 0.0f, 5.0f);
    recordView.cameraForward = Vec3(0.0f, 0.0f, -1.0f);
    recordView.inverseViewMatrix = Mat4Identity();

    ShadowPassConfig config;
    config.numCascades = 3;
    config.shadowMapSize = 72;
    ShadowPass pass;
    ConfigureResources(pass, gpuResources, pipelineCache);
    pass.SetConfig(config);
    pass.SetEnabled(true);
    RenderPassRecordContext context = MakeMainSceneRecordContext(
        graph, recordView, emptyScene, emptyDrawItems, emptyDrawItems,
        compiled.plan, preparation, report, 1821);
    context.primaryDirectionalLight = MakeShadowPrimaryLight();
    context.frameSnapshot = MakeRenderPassFrameSnapshot(
        context, *context.results);
    context.results->shadowStats.drawCount = 59;
    context.results->shadowStats.resolvedCascadeViewCount = 59;
    pass.AddToGraph(graph, context);

    ASSERT_TRUE(context.results->directionalShadowOutput.enabled);
    EXPECT_TRUE(context.results->directionalShadowOutput.IsCompatibleWith(
        context.identity));
    EXPECT_TRUE(HasCurrentGraphProvenance(
        context.results->directionalShadowOutput.shadowMap, context.identity));
    EXPECT_EQ(config.numCascades,
              context.results->shadowStats.configuredCascadeCount);
    EXPECT_EQ(config.numCascades,
              context.results->shadowStats.declaredCascadeResourceCount);

    graph.Compile();
    ASSERT_TRUE(graph.GetCompileStats().compileValid);
    RecordingCommandContext commands;
    graph.Execute(commands);

    EXPECT_EQ(config.numCascades, commands.beginRenderPassCount);
    EXPECT_EQ(config.numCascades, commands.endRenderPassCount);
    EXPECT_EQ(0u, commands.drawIndexedCount);
    EXPECT_EQ(config.numCascades,
              context.results->shadowStats.resolvedCascadeViewCount);
    EXPECT_EQ(0u, context.results->shadowStats.shadowCasterCount);
    EXPECT_EQ(0u, context.results->shadowStats.drawCount);
}

TEST_F(RenderPassValidationFixture,
       ShadowPassB2bRetainsEveryCascadeAttachmentAndRejectsStaleRecord)
{
    RVX_REQUIRE_RENDER_RUNTIME_PIPELINE();
    device.SetFenceAutoComplete(false);

    RenderSubmissionTracker tracker;
    ASSERT_TRUE(tracker.Initialize(&device));
    RenderRetirementQueue retirement;
    ASSERT_TRUE(retirement.Initialize(&tracker));
    RenderSubmissionResourceBatch batch;
    RenderScene emptyScene;
    const std::vector<RenderDrawItem> emptyDrawItems;
    const SceneMeshPassPreparation preparation = PrepareOpaquePackets(
        emptyDrawItems, emptyDrawItems);
    const RenderFramePlanCompileResult compiled = CompileForcedDirectPlan(
        preparation, 1831);
    ASSERT_TRUE(compiled.succeeded);
    RenderFrameExecutionReport report = MakeExecutionReport(compiled.plan);

    const auto makeShadowView = [this](RenderGraph& graph,
                                        RenderSubmissionResourceBatch* batchOwner)
    {
        ViewData recordView;
        recordView.renderGraph = &graph;
        recordView.viewCache = &viewCache;
        recordView.submissionResourceBatch = batchOwner;
        recordView.viewportWidth = 96;
        recordView.viewportHeight = 64;
        recordView.aspectRatio = 1.5f;
        recordView.fieldOfView = 1.0472f;
        recordView.nearPlane = 0.1f;
        recordView.farPlane = 100.0f;
        recordView.cameraPosition = Vec3(0.0f, 0.0f, 5.0f);
        recordView.cameraForward = Vec3(0.0f, 0.0f, -1.0f);
        recordView.inverseViewMatrix = Mat4Identity();
        return recordView;
    };

    ShadowPassConfig config;
    config.numCascades = 2;
    config.shadowMapSize = 80;
    ShadowPass pass;
    ConfigureResources(pass, gpuResources, pipelineCache);
    pass.SetConfig(config);
    pass.SetEnabled(true);

    RenderGraph graph;
    graph.SetDevice(&device);
    ViewData recordView = makeShadowView(graph, &batch);
    RenderPassRecordContext context = MakeMainSceneRecordContext(
        graph, recordView, emptyScene, emptyDrawItems, emptyDrawItems,
        compiled.plan, preparation, report, 1831, &batch);
    context.primaryDirectionalLight = MakeShadowPrimaryLight();
    context.frameSnapshot = MakeRenderPassFrameSnapshot(
        context, *context.results);
    pass.AddToGraph(graph, context);
    graph.Compile();
    ASSERT_TRUE(graph.GetCompileStats().compileValid);
    RecordingCommandContext commands;
    graph.Execute(commands);

    ASSERT_EQ(config.numCascades, commands.renderPasses.size());
    ASSERT_EQ(config.numCascades,
              context.results->shadowStats.resolvedCascadeViewCount);
    ASSERT_EQ(0u, context.results->shadowStats.drawCount);
    RHITexture* const shadowTexture = graph.GetTexture(
        context.results->directionalShadowOutput.shadowMap);
    ASSERT_NE(nullptr, shadowTexture);
    RHITextureRef shadowTextureProbe(shadowTexture);
    ASSERT_TRUE(shadowTextureProbe);
    std::vector<RHITextureViewRef> cascadeViewProbes;
    cascadeViewProbes.reserve(commands.renderPasses.size());
    for (uint32 i = 0; i < config.numCascades; ++i)
    {
        const RHIRenderPassDesc& renderPass = commands.renderPasses[i];
        ASSERT_TRUE(renderPass.hasDepthStencil);
        ASSERT_NE(nullptr, renderPass.depthStencilAttachment.view);
        EXPECT_EQ(shadowTexture, renderPass.depthStencilAttachment.view->GetTexture());
        EXPECT_EQ(config.shadowMapSize,
                  renderPass.depthStencilAttachment.view->GetTexture()->GetWidth());
        EXPECT_EQ(i,
                  renderPass.depthStencilAttachment.view->GetSubresourceRange().baseArrayLayer);
        cascadeViewProbes.emplace_back(renderPass.depthStencilAttachment.view);
    }
    ASSERT_EQ(config.numCascades, cascadeViewProbes.size());

    GPUCompletionToken completion;
    ASSERT_TRUE(InsertGPUCompletionPoint(completion, tracker.Submit(&commands)));
    graph.Clear();
    viewCache.Clear();
    for (const RHITextureViewRef& cascadeViewProbe : cascadeViewProbes)
    {
        ASSERT_TRUE(cascadeViewProbe);
        EXPECT_EQ(2u, cascadeViewProbe->GetRefCount());
    }
    EXPECT_EQ(2u, shadowTextureProbe->GetRefCount());

    RenderGraph staleGraph;
    staleGraph.SetDevice(&device);
    ViewData staleView = makeShadowView(staleGraph, nullptr);
    RenderFrameExecutionReport staleReport = MakeExecutionReport(compiled.plan);
    RenderPassRecordContext stale = MakeMainSceneRecordContext(
        staleGraph, staleView, emptyScene, emptyDrawItems, emptyDrawItems,
        compiled.plan, preparation, staleReport, 1832);
    const RGTextureHandle staleShadow = staleGraph.CreateTexture(
        RHITextureDesc::DepthStencil(16, 16,
                                     PipelineCache::GetDefaultDepthStencilFormat()));
    stale.results->directionalShadowOutput.enabled = true;
    stale.results->directionalShadowOutput.shadowMap = staleShadow;
    stale.results->directionalShadowOutput.shadowMapSize = 16;
    stale.results->directionalShadowOutput.cascadeViewProjections = {Mat4Identity()};
    stale.results->directionalShadowOutput.cascadeSplitDepths = {1.0f};
    stale.results->shadowStats.drawCount = 83;
    stale.results->shadowStats.resolvedCascadeViewCount = 84;
    stale.results->executionReport.status = RenderExecutionStatus::Completed;
    const DirectionalShadowRecordOutput staleOutputBefore =
        stale.results->directionalShadowOutput;
    const ShadowPassStats staleStatsBefore = stale.results->shadowStats;
    const RenderFrameExecutionReport staleReportBefore =
        stale.results->executionReport;
    staleGraph.Clear();
    const size_t texturesBeforeStaleRegistration = device.createdTextureDescs.size();
    pass.AddToGraph(staleGraph, stale);
    staleGraph.Compile();
    ASSERT_TRUE(staleGraph.GetCompileStats().compileValid);
    ASSERT_EQ(1u, staleGraph.GetDiagnostics().passes.size());
    EXPECT_TRUE(staleGraph.GetDiagnostics().passes[0].usages.empty());
    RecordingCommandContext staleCommands;
    staleGraph.Execute(staleCommands);
    EXPECT_EQ(0u, staleCommands.beginRenderPassCount);
    EXPECT_EQ(texturesBeforeStaleRegistration, device.createdTextureDescs.size());
    EXPECT_EQ(staleOutputBefore.identity,
              stale.results->directionalShadowOutput.identity);
    EXPECT_EQ(staleOutputBefore.enabled,
              stale.results->directionalShadowOutput.enabled);
    EXPECT_EQ(staleOutputBefore.shadowMap.index,
              stale.results->directionalShadowOutput.shadowMap.index);
    EXPECT_EQ(staleOutputBefore.shadowMap.graphIdentity,
              stale.results->directionalShadowOutput.shadowMap.graphIdentity);
    EXPECT_EQ(staleOutputBefore.shadowMap.recordingGeneration,
              stale.results->directionalShadowOutput.shadowMap.recordingGeneration);
    EXPECT_EQ(staleOutputBefore.shadowMap.hasSubresourceRange,
              stale.results->directionalShadowOutput.shadowMap.hasSubresourceRange);
    EXPECT_EQ(staleOutputBefore.shadowMapSize,
              stale.results->directionalShadowOutput.shadowMapSize);
    EXPECT_EQ(staleStatsBefore.drawCount, stale.results->shadowStats.drawCount);
    EXPECT_EQ(staleStatsBefore.resolvedCascadeViewCount,
              stale.results->shadowStats.resolvedCascadeViewCount);
    EXPECT_EQ(staleReportBefore.status, stale.results->executionReport.status);
    EXPECT_EQ(staleReportBefore.frameSequence,
              stale.results->executionReport.frameSequence);

    batch.SealAndTransfer(completion, retirement);
    FakeFence* const graphicsFence =
        device.FindFenceWithSignal(completion.points[0].value);
    ASSERT_NE(nullptr, graphicsFence);
    graphicsFence->Complete(completion.points[0].value);
    EXPECT_EQ(GPUCompletionStatus::Completed, retirement.Poll());
    for (const RHITextureViewRef& cascadeViewProbe : cascadeViewProbes)
    {
        EXPECT_EQ(1u, cascadeViewProbe->GetRefCount());
    }
    EXPECT_EQ(1u, shadowTextureProbe->GetRefCount());
    tracker.Shutdown();
}

#undef RVX_REQUIRE_RENDER_RUNTIME_PIPELINE
