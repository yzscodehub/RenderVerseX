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

#include "Render/Debug/DebugRenderer.h"
#include "Render/Decal/DecalRenderer.h"
#include "Render/GPUResourceManager.h"
#include "Render/GPUDriven/GPUCulling.h"
#include "Render/Graph/ResourceViewCache.h"
#include "Render/Material/MaterialSystem.h"
#include "Render/Passes/DepthPrepass.h"
#include "Render/Passes/IRenderPass.h"
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
#include "Render/SwapChainManager.h"
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
    namespace RTShadowBindings = RayTracingResourceBindings::Shadow;
    namespace RTReflectionBindings = RayTracingResourceBindings::Reflection;

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
        explicit RecordingCommandContext(
            RHICommandQueueType queueType = RHICommandQueueType::Graphics)
            : m_queueType(queueType)
        {
        }

        RHICommandQueueType GetQueueType() const override
        {
            return m_queueType;
        }

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
        std::vector<RHIViewport> viewports;
        std::vector<RHIRect> scissors;
        std::vector<std::string> callSequence;

    private:
        RHICommandQueueType m_queueType = RHICommandQueueType::Graphics;
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

        RHIPipelineRef CreateGraphicsPipeline(const RHIGraphicsPipelineDesc& desc) override
        {
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
            return RHIDescriptorSetRef(new FakeDescriptorSet(desc));
        }

        RHIQueryPoolRef CreateQueryPool(const RHIQueryPoolDesc&) override { return nullptr; }

        RHICommandContextRef CreateCommandContext(
            RHICommandQueueType queueType) override
        {
            return RHICommandContextRef(
                new RecordingCommandContext(queueType));
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

        void EnableBasicCapabilities()
        {
            m_capabilities.backendType = RHIBackendType::DX12;
            m_capabilities.adapterName = "RenderPassValidation Test Adapter";
            m_capabilities.driverVersion = "RenderPassValidation.Driver.1";
            m_capabilities.supportsComputePipeline = true;
            m_capabilities.supportsDescriptorSets = true;
            m_capabilities.maxDescriptorSets = 8;
            m_capabilities.supportsExplicitResourceBarriers = true;
            m_capabilities.supportsDefaultQueueFenceSignal = true;
            m_capabilities.supportsAsyncCompute = true;
            m_capabilities.queueTopology.completionMode = RHIQueueCompletionMode::NativeTimeline;
            m_capabilities.queueTopology.logicalQueueDomains = {
                GPUQueueDomain::Graphics,
                GPUQueueDomain::Compute,
                GPUQueueDomain::Copy,
            };
            m_capabilities.queueTopology.activeDomainCount = 3;
            m_capabilities.dx12.resourceBindingTier = 2;
        }

        void EnableRayTracing()
        {
            m_capabilities.backendType = RHIBackendType::DX12;
            m_capabilities.adapterName = "RenderPassValidation Test Adapter";
            m_capabilities.driverVersion = "RenderPassValidation.Driver.1";
            m_capabilities.supportsRaytracing = true;
            m_capabilities.supportsRaytracingPipeline = true;
            m_capabilities.supportsComputePipeline = true;
            m_capabilities.supportsAccelerationStructureUpdate = true;
            m_capabilities.supportsAccelerationStructureCompaction = false;
            m_capabilities.maxRayRecursionDepth = 1;
            m_capabilities.shaderGroupHandleSize = 32;
            m_capabilities.shaderGroupHandleAlignment = 32;
            m_capabilities.shaderTableBaseAlignment = 64;
            m_capabilities.supportsDescriptorSets = true;
            m_capabilities.supportsExplicitResourceBarriers = true;
            m_capabilities.maxDescriptorSets = 8;
            m_capabilities.supportsDefaultQueueFenceSignal = true;
            m_capabilities.supportsAsyncCompute = true;
            m_capabilities.queueTopology.completionMode = RHIQueueCompletionMode::NativeTimeline;
            m_capabilities.queueTopology.logicalQueueDomains = {
                GPUQueueDomain::Graphics,
                GPUQueueDomain::Compute,
                GPUQueueDomain::Copy,
            };
            m_capabilities.queueTopology.activeDomainCount = 3;
            m_capabilities.dx12.resourceBindingTier = 2;
        }

        bool bufferMapSucceeds = true;
        bool textureViewCreationSucceeds = true;
        bool failDirectionalShadowSRVCreation = false;
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
        PostProcessFrameInputRequirements GetFrameInputRequirements() const override { return requirements; }

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
        const auto it = std::find_if(report.features.begin(),
                                     report.features.end(),
                                     [feature](const SceneRenderFeatureCapability& candidate)
                                     {
                                         return candidate.feature == feature;
                                     });
        return it != report.features.end() ? &(*it) : nullptr;
    }

    void PrepareRayTracingSceneForSingleObject(FakeDevice& device,
                                               GPUResourceManager& gpuResources,
                                               const RenderScene& scene,
                                               RayTracingSceneManager& sceneManager)
    {
        std::vector<uint32_t> visibleObjectIndices{0};
        RayTracingSceneBuildPlan plan = BuildRayTracingSceneBuildPlan(scene, visibleObjectIndices, gpuResources);
        ASSERT_TRUE(plan.HasWork());

        sceneManager.Initialize(&device);
        ASSERT_TRUE(sceneManager.IsSupported());
        ASSERT_TRUE(sceneManager.Prepare(plan)) << sceneManager.GetStats().fallbackReason;
        EXPECT_EQ(sceneManager.GetStats().fallbackCode, RayTracingSceneFallbackCode::None);
        EXPECT_STREQ(sceneManager.GetStats().fallbackReason, "");

        RecordingCommandContext buildCtx;
        sceneManager.RecordBuildCommands(buildCtx);
        EXPECT_GE(buildCtx.buildBottomLevelASCount, 1u);
        EXPECT_EQ(buildCtx.buildTopLevelASCount, 1u);
        EXPECT_NE(sceneManager.GetTopLevelAS(), nullptr);
        EXPECT_NE(sceneManager.GetInstanceMaterialMetadataBuffer(), nullptr);
        EXPECT_NE(sceneManager.GetInstanceAlphaMetadataBuffer(), nullptr);
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

        void Initialize(bool bufferMapSucceeds = true, bool rayTracingSupported = false)
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
            else
            {
                device.EnableBasicCapabilities();
            }

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
    ASSERT_NO_FATAL_FAILURE(Initialize());

    ShadowPass pass;
    pass.SetResources(&gpuResources, &pipelineCache);
    pass.SetDirectionalLight(Vec3{0.0f, -1.0f, 0.0f}, Vec3{1.0f, 1.0f, 1.0f}, 1.0f);

    EXPECT_TRUE(pass.IsRequestedEnabled());
    EXPECT_TRUE(pass.IsSupported()) << pass.GetUnsupportedReason();
    EXPECT_TRUE(pass.IsEnabled());

    pipelineCache.m_pipelineCache.clear();
    pipelineCache.m_depthOnlyVertexShader.Reset();
    EXPECT_FALSE(pass.IsSupported());
    EXPECT_FALSE(pass.GetUnsupportedReason().empty());
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

TEST_F(RenderPassValidationFixture, RayTracedShadowPassRuntimeCreatesDescriptorSetAndDispatchesRays)
{
    ASSERT_NO_FATAL_FAILURE(Initialize(true, true));

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

    RayTracedShadowPass pass;
    pass.SetEnabled(true);
    pass.OnAdd(&device);
    pass.SetResources(&gpuResources, &pipelineCache, &viewCache);
    pass.SetRayTracingScene(&rayTracingScene);
    pass.SetDirectionalLight(Vec3{-0.3f, -1.0f, -0.2f}, Vec3{1.0f, 1.0f, 1.0f}, 1.0f);

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

    const RayTracedShadowPassStats& stats = pass.GetStats();
    EXPECT_TRUE(stats.outputDeclared);
    EXPECT_TRUE(stats.resourceViewsAvailable);
    EXPECT_TRUE(stats.descriptorSetAvailable);
    EXPECT_TRUE(stats.constantsUploaded);
    EXPECT_TRUE(stats.dispatchRecorded);
    EXPECT_TRUE(stats.historyAvailable);
    EXPECT_EQ(stats.dispatchPixelCount, 64u * 64u);
}

TEST_F(RenderPassValidationFixture, RayTracedShadowPassRejectsOversizedMaterialTextureTable)
{
    ASSERT_NO_FATAL_FAILURE(Initialize(true, true));

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

        RenderObject object = MakeRenderObject(*meshResource);
        object.entityId = 13000 + i;
        object.materialIds = {material->GetId()};
        object.materialResources = {material.get()};
        oversizedScene.AddObject(object);
        visibleObjectIndices.push_back(i);

        textures.push_back(albedo);
        materials.push_back(std::move(material));
    }

    RayTracingSceneBuildPlan plan = BuildRayTracingSceneBuildPlan(
        oversizedScene,
        visibleObjectIndices,
        gpuResources);
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
    pass.SetResources(&gpuResources, &pipelineCache, &viewCache);
    pass.SetRayTracingScene(&rayTracingScene);
    pass.SetDirectionalLight(Vec3{-0.3f, -1.0f, -0.2f}, Vec3{1.0f, 1.0f, 1.0f}, 1.0f);

    EXPECT_FALSE(pass.IsSupported());
    EXPECT_EQ(pass.GetUnsupportedReason(),
              "Ray tracing material texture table exceeds the supported descriptor count");

    RenderGraph graph;
    graph.SetDevice(&device);
    pass.AddToGraph(graph, view);

    const RayTracedShadowPassStats& stats = pass.GetStats();
    EXPECT_FALSE(stats.supported);
    EXPECT_EQ(stats.materialTextureCount, kMaterialTextureLimit);
    EXPECT_FALSE(stats.materialTextureTableAvailable);
    EXPECT_TRUE(stats.alphaTextureTableAvailable);
    EXPECT_TRUE(stats.alphaGeometryTableAvailable);
    EXPECT_FALSE(stats.outputDeclared);
}

TEST_F(RenderPassValidationFixture, RayTracedShadowPassRejectsOversizedAlphaTextureTable)
{
    ASSERT_NO_FATAL_FAILURE(Initialize(true, true));

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

        RenderObject object = MakeRenderObject(*meshResource);
        object.entityId = 16000 + i;
        object.materialIds = {material->GetId()};
        object.materialResources = {material.get()};
        oversizedScene.AddObject(object);
        visibleObjectIndices.push_back(i);

        textures.push_back(albedo);
        materials.push_back(std::move(material));
    }

    RayTracingSceneBuildPlan plan = BuildRayTracingSceneBuildPlan(
        oversizedScene,
        visibleObjectIndices,
        gpuResources);
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
    pass.SetResources(&gpuResources, &pipelineCache, &viewCache);
    pass.SetRayTracingScene(&rayTracingScene);
    pass.SetDirectionalLight(Vec3{-0.3f, -1.0f, -0.2f}, Vec3{1.0f, 1.0f, 1.0f}, 1.0f);

    EXPECT_FALSE(pass.IsSupported());
    EXPECT_EQ(pass.GetUnsupportedReason(),
              "Ray tracing alpha texture table exceeds the supported descriptor count");

    RenderGraph graph;
    graph.SetDevice(&device);
    pass.AddToGraph(graph, view);

    const RayTracedShadowPassStats& stats = pass.GetStats();
    EXPECT_FALSE(stats.supported);
    EXPECT_TRUE(stats.materialTextureTableAvailable);
    EXPECT_EQ(stats.alphaTextureCount, kAlphaTextureLimit);
    EXPECT_FALSE(stats.alphaTextureTableAvailable);
    EXPECT_TRUE(stats.alphaGeometryTableAvailable);
    EXPECT_FALSE(stats.outputDeclared);
}

TEST_F(RenderPassValidationFixture, RayTracedShadowPassRejectsOversizedAlphaGeometryBufferTable)
{
    ASSERT_NO_FATAL_FAILURE(Initialize(true, true));

    constexpr uint32 kAlphaGeometryBufferLimit = RTShadowBindings::RVX_RT_SHADOW_MAX_ALPHA_GEOMETRY_BUFFERS;
    constexpr uint32 kMeshCount = kAlphaGeometryBufferLimit + 1;

    auto material = std::make_unique<Resource::MaterialResource>();
    material->SetId(17000);
    material->SetName("RayTracedShadowAlphaGeometryOverflowMaterial");
    auto materialData = std::make_shared<Material>("RayTracedShadowAlphaGeometryOverflowMaterial");
    materialData->SetAlphaMode(Material::AlphaMode::Mask);
    material->SetMaterialData(materialData);

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

        RenderObject object = MakeRenderObject(*mesh);
        object.entityId = 19000 + i;
        object.materialIds = {material->GetId()};
        object.materialResources = {material.get()};
        oversizedScene.AddObject(object);
        visibleObjectIndices.push_back(i);

        meshes.push_back(std::move(mesh));
    }

    RayTracingSceneBuildPlan plan = BuildRayTracingSceneBuildPlan(
        oversizedScene,
        visibleObjectIndices,
        gpuResources);
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
    pass.SetResources(&gpuResources, &pipelineCache, &viewCache);
    pass.SetRayTracingScene(&rayTracingScene);
    pass.SetDirectionalLight(Vec3{-0.3f, -1.0f, -0.2f}, Vec3{1.0f, 1.0f, 1.0f}, 1.0f);

    EXPECT_FALSE(pass.IsSupported());
    EXPECT_EQ(pass.GetUnsupportedReason(),
              "Ray tracing alpha geometry buffer table exceeds the supported descriptor count");

    RenderGraph graph;
    graph.SetDevice(&device);
    pass.AddToGraph(graph, view);

    const RayTracedShadowPassStats& stats = pass.GetStats();
    EXPECT_FALSE(stats.supported);
    EXPECT_TRUE(stats.materialTextureTableAvailable);
    EXPECT_TRUE(stats.alphaTextureTableAvailable);
    EXPECT_EQ(stats.alphaIndexBufferCount, kAlphaGeometryBufferLimit);
    EXPECT_EQ(stats.alphaUVBufferCount, kAlphaGeometryBufferLimit);
    EXPECT_FALSE(stats.alphaGeometryTableAvailable);
    EXPECT_FALSE(stats.outputDeclared);
}

TEST_F(RenderPassValidationFixture, RayTracedReflectionPassRuntimeCreatesDescriptorSetAndDispatchesRays)
{
    ASSERT_NO_FATAL_FAILURE(Initialize(true, true));

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
    pass.SetResources(&gpuResources, &pipelineCache, &viewCache);
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
    ASSERT_NO_FATAL_FAILURE(Initialize(true, true));

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

        RenderObject object = MakeRenderObject(*meshResource);
        object.entityId = 8000 + i;
        object.materialIds = {material->GetId()};
        object.materialResources = {material.get()};
        oversizedScene.AddObject(object);
        visibleObjectIndices.push_back(i);

        textures.push_back(albedo);
        materials.push_back(std::move(material));
    }

    RayTracingSceneBuildPlan plan = BuildRayTracingSceneBuildPlan(
        oversizedScene,
        visibleObjectIndices,
        gpuResources);
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
    pass.SetResources(&gpuResources, &pipelineCache, &viewCache);
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
    ASSERT_NO_FATAL_FAILURE(Initialize(true, true));

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

        RenderObject object = MakeRenderObject(*mesh);
        object.entityId = 10000 + i;
        oversizedScene.AddObject(object);
        visibleObjectIndices.push_back(i);

        meshes.push_back(std::move(mesh));
    }

    RayTracingSceneBuildPlan plan = BuildRayTracingSceneBuildPlan(
        oversizedScene,
        visibleObjectIndices,
        gpuResources);
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
    pass.SetResources(&gpuResources, &pipelineCache, &viewCache);
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
    ASSERT_NO_FATAL_FAILURE(Initialize(true, true));

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
    reflectionPass.SetResources(&gpuResources, &pipelineCache, &viewCache);
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
    ASSERT_NO_FATAL_FAILURE(Initialize(true, true));

    RayTracingSceneManager rayTracingScene;
    ASSERT_NO_FATAL_FAILURE(PrepareRayTracingSceneForSingleObject(device, gpuResources, scene, rayTracingScene));

    RHITextureDesc depthDesc = RHITextureDesc::DepthStencil(64, 64, PipelineCache::GetDefaultDepthStencilFormat());
    depthDesc.debugName = "RayTracedShadowStableHistoryDepth";
    RHITextureRef depthTexture = device.CreateTexture(depthDesc);
    ASSERT_TRUE(depthTexture);

    RayTracedShadowPass pass;
    pass.SetEnabled(true);
    pass.OnAdd(&device);
    pass.SetResources(&gpuResources, &pipelineCache, &viewCache);
    pass.SetRayTracingScene(&rayTracingScene);
    pass.SetDirectionalLight(Vec3{-0.3f, -1.0f, -0.2f}, Vec3{1.0f, 1.0f, 1.0f}, 1.0f);

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
    };

    RecordingCommandContext firstCtx;
    runFrame(1, firstCtx);
    EXPECT_EQ(firstCtx.dispatchRaysCount, 1u);
    EXPECT_TRUE(firstCtx.lastDispatchRaysValidation.valid) << firstCtx.lastDispatchRaysValidation.message;
    const RayTracedShadowPassStats firstStats = pass.GetStats();
    EXPECT_TRUE(firstStats.dispatchRecorded);
    EXPECT_TRUE(firstStats.historyRecreated);
    EXPECT_TRUE(firstStats.historyAvailable);
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
    EXPECT_EQ(device.createdTextureDescs.size(), textureCountAfterFirstFrame);
}

TEST_F(RenderPassValidationFixture, RayTracedReflectionPassReusesHistoryAcrossStableFrames)
{
    ASSERT_NO_FATAL_FAILURE(Initialize(true, true));

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
    pass.SetResources(&gpuResources, &pipelineCache, &viewCache);
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

TEST_F(RenderPassValidationFixture, ShadowPassStabilizesCascadeCentersToShadowTexels)
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
    view.cameraPosition = Vec3(0.37f, 0.0f, 5.19f);
    view.cameraForward = Vec3(0.0f, 0.0f, -1.0f);
    view.inverseViewMatrix = Mat4Identity();

    ShadowPassConfig config;
    config.numCascades = 3;
    config.shadowMapSize = 128;
    config.stabilizeCascades = true;

    ShadowPass stabilizedPass;
    stabilizedPass.SetResources(&gpuResources, &pipelineCache);
    stabilizedPass.SetRenderScene(&scene);
    stabilizedPass.SetConfig(config);
    stabilizedPass.SetDirectionalLight(Vec3{-0.4f, -1.0f, -0.2f}, Vec3{1.0f, 1.0f, 1.0f}, 2.0f);
    stabilizedPass.AddToGraph(graph, view);

    ASSERT_EQ(stabilizedPass.GetCascades().size(), static_cast<size_t>(3));
    for (const ShadowCascade& cascade : stabilizedPass.GetCascades())
    {
        EXPECT_GT(cascade.stableExtent, 0.0f);
        EXPECT_GT(cascade.texelWorldSize, 0.0f);
        EXPECT_TRUE(IsAlignedToTexel(cascade.lightSpaceCenter.x, cascade.texelWorldSize));
        EXPECT_TRUE(IsAlignedToTexel(cascade.lightSpaceCenter.y, cascade.texelWorldSize));
    }

    RenderGraph unsnappedGraph;
    unsnappedGraph.SetDevice(&device);
    view.renderGraph = &unsnappedGraph;
    config.stabilizeCascades = false;

    ShadowPass unsnappedPass;
    unsnappedPass.SetResources(&gpuResources, &pipelineCache);
    unsnappedPass.SetRenderScene(&scene);
    unsnappedPass.SetConfig(config);
    unsnappedPass.SetDirectionalLight(Vec3{-0.4f, -1.0f, -0.2f}, Vec3{1.0f, 1.0f, 1.0f}, 2.0f);
    unsnappedPass.AddToGraph(unsnappedGraph, view);

    ASSERT_EQ(unsnappedPass.GetCascades().size(), stabilizedPass.GetCascades().size());
    const Vec2 stabilizedCenter = stabilizedPass.GetCascades()[0].lightSpaceCenter;
    const Vec2 unsnappedCenter = unsnappedPass.GetCascades()[0].lightSpaceCenter;
    EXPECT_GT(length(stabilizedCenter - unsnappedCenter), 0.0001f);
}

TEST_F(RenderPassValidationFixture, ShadowPassStableCascadeIgnoresSubTexelCameraMotion)
{
    ASSERT_NO_FATAL_FAILURE(Initialize());

    auto buildFirstCascade = [&](const ViewData& inputView)
    {
        RenderGraph graph;
        graph.SetDevice(&device);

        ViewData localView = inputView;
        localView.renderGraph = &graph;
        localView.viewCache = &viewCache;

        ShadowPassConfig config;
        config.numCascades = 3;
        config.shadowMapSize = 128;
        config.stabilizeCascades = true;

        ShadowPass pass;
        pass.SetResources(&gpuResources, &pipelineCache);
        pass.SetRenderScene(&scene);
        pass.SetConfig(config);
        pass.SetDirectionalLight(Vec3{-0.4f, -1.0f, -0.2f}, Vec3{1.0f, 1.0f, 1.0f}, 2.0f);
        pass.AddToGraph(graph, localView);

        EXPECT_FALSE(pass.GetCascades().empty());
        return pass.GetCascades()[0];
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

    const ShadowCascade baseCascade = buildFirstCascade(baseView);
    ASSERT_GT(baseCascade.texelWorldSize, 0.0f);
    const Vec3 fixedWorldPoint = baseView.cameraPosition + baseView.cameraForward * 1.0f;
    const Vec2 baseShadowUV = ProjectShadowUV(baseCascade, fixedWorldPoint);

    ViewData smallMoveView = baseView;
    smallMoveView.cameraPosition.x += baseCascade.texelWorldSize * 0.1f;
    const ShadowCascade smallMoveCascade = buildFirstCascade(smallMoveView);
    EXPECT_NEAR(smallMoveCascade.lightSpaceCenter.x, baseCascade.lightSpaceCenter.x, 0.0001f);
    EXPECT_NEAR(smallMoveCascade.lightSpaceCenter.y, baseCascade.lightSpaceCenter.y, 0.0001f);
    const Vec2 smallMoveShadowUV = ProjectShadowUV(smallMoveCascade, fixedWorldPoint);
    EXPECT_NEAR(smallMoveShadowUV.x, baseShadowUV.x, 0.00001f);
    EXPECT_NEAR(smallMoveShadowUV.y, baseShadowUV.y, 0.00001f);

    ViewData largeMoveView = baseView;
    largeMoveView.cameraPosition.x += baseCascade.texelWorldSize * 8.0f;
    const ShadowCascade largeMoveCascade = buildFirstCascade(largeMoveView);
    EXPECT_GT(length(largeMoveCascade.lightSpaceCenter - baseCascade.lightSpaceCenter), 0.0f);
    const Vec2 largeMoveShadowUV = ProjectShadowUV(largeMoveCascade, fixedWorldPoint);
    EXPECT_GT(length(largeMoveShadowUV - baseShadowUV), 0.001f);
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
    config.casterDepthBias = 1.25f;
    config.casterSlopeScaledDepthBias = 2.0f;
    config.casterDepthBiasClamp = 0.5f;

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
    EXPECT_EQ(diagnostics.schemaVersion, RVX_SCENE_RENDERER_FRAME_DIAGNOSTICS_SCHEMA_VERSION);
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
    EXPECT_EQ(diagnostics.gpuResourceStats.residentMeshCount, static_cast<size_t>(0));
    EXPECT_EQ(diagnostics.gpuResourceStats.residentTextureCount, static_cast<size_t>(0));
    EXPECT_EQ(diagnostics.gpuResourceStats.pendingUploadCount, static_cast<size_t>(0));
    EXPECT_EQ(diagnostics.gpuResourceStats.failedUploadCount, static_cast<size_t>(0));
    EXPECT_EQ(diagnostics.gpuResourceStats.usedMemory, static_cast<size_t>(0));
    EXPECT_EQ(diagnostics.gpuResourceStats.memoryBudget, static_cast<size_t>(0));
    EXPECT_FALSE(diagnostics.gpuDrivenCullingStats.enabled);
    EXPECT_FALSE(diagnostics.gpuDrivenCullingStats.executionDecisionAvailable);
    EXPECT_EQ(diagnostics.gpuDrivenCullingStats.inputOpaqueDrawItemCount, 0u);
    EXPECT_EQ(diagnostics.gpuDrivenCullingStats.graphInputDrawItemCount, 0u);
    EXPECT_FALSE(diagnostics.rayTracingSceneStats.prepared);
    EXPECT_FALSE(diagnostics.rayTracingSceneStats.hasTopLevelAS);
    EXPECT_EQ(diagnostics.rayTracingSceneStats.fallbackCode, RayTracingSceneFallbackCode::None);
    EXPECT_STREQ(diagnostics.rayTracingSceneStats.fallbackReason, "");
    EXPECT_TRUE(diagnostics.postProcessEffectPlans.empty());
    EXPECT_TRUE(diagnostics.graphDiagnostics.empty());

    const SceneRendererToolDiagnosticsSnapshot& toolSnapshot = renderer.GetToolDiagnosticsSnapshot();
    EXPECT_EQ(toolSnapshot.schemaVersion, RVX_SCENE_RENDERER_TOOL_DIAGNOSTICS_SCHEMA_VERSION);
    EXPECT_TRUE(toolSnapshot.frameDiagnosticsAvailable);
    EXPECT_TRUE(toolSnapshot.renderGraphDiagnosticsAvailable);
    EXPECT_EQ(toolSnapshot.frame.frameCount, diagnostics.frameCount);
    EXPECT_TRUE(toolSnapshot.frame.graphBuilt);
    EXPECT_TRUE(toolSnapshot.frame.externalTargetActive);
    EXPECT_GE(toolSnapshot.renderGraph.resources.size(), static_cast<size_t>(2));
    EXPECT_EQ(toolSnapshot.renderGraph.resources.size(), renderer.GetRenderGraph()->GetDiagnostics().resources.size());
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
    EXPECT_EQ(toolSnapshot.rhiCapabilityReport.entries.size(), static_cast<size_t>(11));

    const std::string diagnosticsText = renderer.ExportToolDiagnosticsText();
    EXPECT_NE(diagnosticsText.find("rhiCapabilities=true"), std::string::npos);
    EXPECT_NE(diagnosticsText.find("RHICapabilities: schema=4, backend=DirectX 12"), std::string::npos);
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
    EXPECT_NE(manifestJson.find("\"rhiCapabilities\": {\n    \"schemaVersion\": 4"), std::string::npos);
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
    EXPECT_NE(rhiCapabilityJson.find("\"schemaVersion\": 4"), std::string::npos);
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
    EXPECT_NE(artifactManifestJson.find("\"schemaVersion\": 3"), std::string::npos);
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
    EXPECT_NE(renderGraphDiagnosticsJson.find("\"schemaVersion\": 3"), std::string::npos);
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
    EXPECT_NE(manifestJson.find("\"schemaVersion\": 24"), std::string::npos);
    EXPECT_NE(manifestJson.find("\"id\": \"manifestJson\""), std::string::npos);
    EXPECT_NE(manifestJson.find("\"kind\": \"ToolDiagnosticsManifestJson\""), std::string::npos);
    EXPECT_NE(manifestJson.find("\"contentType\": \"application/json\""), std::string::npos);
    EXPECT_NE(manifestJson.find("\"contentHash\": \"\""), std::string::npos);
    EXPECT_NE(manifestJson.find("\"relativePath\": \"\""), std::string::npos);
    EXPECT_NE(manifestJson.find("\"frameDiagnosticsAvailable\": true"), std::string::npos);
    EXPECT_NE(manifestJson.find("\"renderGraphDiagnosticsAvailable\": true"), std::string::npos);
    EXPECT_NE(manifestJson.find("\"rhiCapabilityReportAvailable\": false"), std::string::npos);
    EXPECT_NE(manifestJson.find("\"rhiCapabilities\": null"), std::string::npos);
    EXPECT_NE(manifestJson.find("\"renderGraph\": {\n    \"schemaVersion\": 3"), std::string::npos);
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
    EXPECT_NE(emptyArtifactSummaryJson.find("\"toolDiagnosticsSchemaVersion\": 24"), std::string::npos);
    EXPECT_NE(emptyArtifactSummaryJson.find("\"artifactResultAvailable\": false"), std::string::npos);
    EXPECT_NE(emptyArtifactSummaryJson.find("\"metadataAvailable\": false"), std::string::npos);
    EXPECT_NE(emptyArtifactSummaryJson.find("\"captureId\": \"\""), std::string::npos);
    EXPECT_NE(emptyArtifactSummaryJson.find("\"baseName\": \"\""), std::string::npos);
    EXPECT_NE(emptyArtifactSummaryJson.find("\"frameIndex\": 0"), std::string::npos);
    EXPECT_NE(emptyArtifactSummaryJson.find("\"renderGraphDiagnosticsSchemaVersion\": 3"), std::string::npos);
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
    EXPECT_NE(artifactSummaryJson.find("\"toolDiagnosticsSchemaVersion\": 24"), std::string::npos);
    EXPECT_NE(artifactSummaryJson.find("\"artifactResultAvailable\": true"), std::string::npos);
    EXPECT_NE(artifactSummaryJson.find("\"capture\": {"), std::string::npos);
    EXPECT_NE(artifactSummaryJson.find("\"metadataAvailable\": true"), std::string::npos);
    EXPECT_NE(artifactSummaryJson.find("\"captureId\": \"" + artifactResult.captureId + "\""),
              std::string::npos);
    EXPECT_NE(artifactSummaryJson.find("\"baseName\": \"Frame001\""), std::string::npos);
    EXPECT_NE(artifactSummaryJson.find("\"outputDirectory\": "), std::string::npos);
    EXPECT_NE(artifactSummaryJson.find("\"frameIndex\": " + std::to_string(toolSnapshot.frame.frameCount)),
              std::string::npos);
    EXPECT_NE(artifactSummaryJson.find("\"renderGraphDiagnosticsSchemaVersion\": 3"), std::string::npos);
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
    EXPECT_NE(artifactSummaryJson.find("\"schemaVersion\": 3"), std::string::npos);
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
    EXPECT_NE(artifactManifestJson.find("\"schemaVersion\": 24"), std::string::npos);
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
    EXPECT_NE(artifactManifestJson.find("\"renderGraphDiagnosticsSchemaVersion\": 3"), std::string::npos);
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
    EXPECT_NE(artifactManifestJson.find("\"schemaVersion\": 3"), std::string::npos);
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
    EXPECT_NE(validationJson.find("\"toolDiagnosticsSchemaVersion\": 24"), std::string::npos);
    EXPECT_NE(validationJson.find("\"renderGraphDiagnosticsSchemaVersion\": 3"), std::string::npos);
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
    EXPECT_NE(validationJson.find("\"schemaVersion\": 3"), std::string::npos);
    EXPECT_NE(validationJson.find("\"actualId\": \"renderGraphDiagnosticsJson\""), std::string::npos);
    EXPECT_NE(validationJson.find("\"actualKind\": \"RenderGraphDiagnosticsJson\""), std::string::npos);
    EXPECT_NE(validationJson.find("\"actualContentType\": \"application/json\""), std::string::npos);
    EXPECT_NE(validationJson.find("\"actualSchemaId\": \"RVX.RenderGraph.Diagnostics\""), std::string::npos);
    EXPECT_NE(validationJson.find("\"actualSchemaVersion\": 3"), std::string::npos);
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
    EXPECT_NE(identityMismatchJson.find("\"actualSchemaVersion\": 3"), std::string::npos);
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
    EXPECT_NE(identityMismatchJson.find("\"primaryFailureArtifactSchemaVersion\": 3"),
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
    EXPECT_NE(modifiedValidationJson.find("\"primaryFailureArtifactSchemaVersion\": 3"),
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
    ASSERT_NO_FATAL_FAILURE(Initialize());

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
    EXPECT_EQ(stats.totalPasses, 7u);
    EXPECT_EQ(stats.culledPasses, 0u);
    EXPECT_EQ(stats.emptyPassUsageCount, 0u);

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

TEST_F(RenderPassValidationFixture, BloomZeroIntensityCopiesSceneOnly)
{
    ASSERT_NO_FATAL_FAILURE(Initialize());

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
    ASSERT_NO_FATAL_FAILURE(Initialize());

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
    ASSERT_EQ(descriptorIt->bindings.size(), static_cast<size_t>(3));
}

TEST_F(RenderPassValidationFixture, FilmGrainUploadsConstantsWithHLSLPacking)
{
    ASSERT_NO_FATAL_FAILURE(Initialize());

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
    ASSERT_NO_FATAL_FAILURE(Initialize(false));

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
    ASSERT_NO_FATAL_FAILURE(Initialize());

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
    settings.enableFilmGrain = true;
    settings.enableFXAA = true;

    PostProcessStack stack;
    auto* bloom = stack.AddEffect<BloomPass>();
    auto* toneMapping = stack.AddEffect<ToneMappingPass>();
    auto* colorGrading = stack.AddEffect<ColorGradingPass>();
    auto* chromaticAberration = stack.AddEffect<ChromaticAberrationPass>();
    auto* vignette = stack.AddEffect<VignettePass>();
    auto* filmGrain = stack.AddEffect<FilmGrainPass>();
    auto* fxaa = stack.AddEffect<FXAAPass>();
    bloom->Configure(settings);
    toneMapping->Configure(settings);
    colorGrading->Configure(settings);
    chromaticAberration->Configure(settings);
    vignette->Configure(settings);
    filmGrain->Configure(settings);
    fxaa->Configure(settings);
    bloom->SetResources(&pipelineCache, &viewCache);
    toneMapping->SetResources(&pipelineCache, &viewCache);
    colorGrading->SetResources(&pipelineCache, &viewCache);
    chromaticAberration->SetResources(&pipelineCache, &viewCache);
    vignette->SetResources(&pipelineCache, &viewCache);
    filmGrain->SetResources(&pipelineCache, &viewCache);
    fxaa->SetResources(&pipelineCache, &viewCache);

    stack.Execute(graph, input, output);

    const PostProcessStackExecuteStats& executeStats = stack.GetLastExecuteStats();
    EXPECT_FALSE(executeStats.noEffectNoWork);
    EXPECT_EQ(executeStats.enabledEffectCount, 7u);
    EXPECT_EQ(executeStats.graphPassCount, 7u);
    EXPECT_EQ(executeStats.transientIntermediateCount, 6u);
    EXPECT_EQ(executeStats.hdrIntermediateCount, 1u);
    EXPECT_EQ(executeStats.hdrIntermediateFormat, RHIFormat::RGBA16_FLOAT);
    EXPECT_EQ(executeStats.ldrIntermediateCount, 5u);
    EXPECT_EQ(executeStats.ldrIntermediateFormat, RHIFormat::RGBA8_UNORM);
    EXPECT_EQ(executeStats.transientIntermediateFormat, RHIFormat::RGBA8_UNORM);
    EXPECT_EQ(executeStats.finalOutputFormat, RHIFormat::RGBA8_UNORM);
    EXPECT_TRUE(executeStats.toneMappingBoundaryValid);
    EXPECT_TRUE(executeStats.toneMappingBoundaryWarning.empty());

    graph.Compile();
    const auto& graphStats = graph.GetCompileStats();
    EXPECT_TRUE(graphStats.compileValid);
    EXPECT_EQ(graphStats.totalPasses, 13u);

    RecordingCommandContext ctx;
    graph.Execute(ctx);

    ASSERT_EQ(ctx.pipelineSequence.size(), static_cast<size_t>(13));
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
    EXPECT_EQ(ctx.pipelineSequence[11], pipelineCache.GetFilmGrainPipeline(RHIFormat::RGBA8_UNORM));
    EXPECT_EQ(ctx.pipelineSequence[12], pipelineCache.GetFXAAPipeline(RHIFormat::RGBA8_UNORM));
    EXPECT_EQ(ctx.drawCount, 13u);
    EXPECT_EQ(ctx.lastDrawVertexCount, 3u);
    EXPECT_EQ(ctx.beginRenderPassCount, 13u);
    EXPECT_EQ(ctx.endRenderPassCount, 13u);
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

TEST_F(RenderPassValidationFixture, PostProcessStackRejectsHDRChainWritingLDROutputWithoutToneMapping)
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
    ASSERT_NO_FATAL_FAILURE(Initialize());

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

    EXPECT_NE(source.find("settings.toneMappingOperator = ToneMappingOperator::ACES;"), std::string::npos);
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

    EXPECT_NE(sceneRendererHeader.find("GPUResourceManager::Stats gpuResourceStats;"),
              std::string::npos);
    EXPECT_NE(sceneRendererSource.find("diagnostics.gpuResourceStats = m_gpuResourceManager->GetStats();"),
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
    EXPECT_NE(sceneRendererHeader.find("std::unique_ptr<RenderFeatureSceneBridge> m_featureBridge;"),
              std::string::npos);
    EXPECT_NE(sceneRendererHeader.find("SceneFeatureExtractionStats featureExtractionStats;"),
              std::string::npos);
    EXPECT_NE(sceneRendererSource.find("m_featureBridge = std::make_unique<RenderFeatureSceneBridge>();"),
              std::string::npos);
    EXPECT_NE(sceneRendererSource.find("std::make_unique<ParticleFeaturePass>()"), std::string::npos);
    EXPECT_NE(sceneRendererSource.find("m_particleFeaturePass->SetSnapshot(&m_featureSnapshot.particles)"),
              std::string::npos);
    EXPECT_NE(sceneRendererSource.find("return m_particleFeaturePass ? m_particleFeaturePass->GetStats()"),
              std::string::npos);
    EXPECT_NE(sceneRendererSource.find("PopulateFeatureExtractionStats("), std::string::npos);
    EXPECT_NE(sceneRendererSource.find("ParticleRenderSnapshotPayloadStatus::MetadataOnly"), std::string::npos);
    EXPECT_NE(sceneRendererSource.find("ParticleRenderSnapshotPayloadStatus::RenderOwnedPayloadReady"),
              std::string::npos);
    EXPECT_NE(sceneRendererSource.find("diagnostics.featureExtractionStats = m_featureExtractionStats;"),
              std::string::npos);
    EXPECT_NE(sceneRendererSource.find("UpdateFeatureExtraction(world);"), std::string::npos);
    EXPECT_NE(sceneRendererSource.find("UpdateFeatureExtraction(sceneManager);"), std::string::npos);
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
    EXPECT_EQ(stats.scheduledEffectCount, 0u);
    EXPECT_EQ(stats.graphPassCount, 0u);
    EXPECT_EQ(stats.requestedQualityPreset, RenderVisualQualityPreset::Medium);
    EXPECT_EQ(stats.appliedQualityPreset, RenderVisualQualityPreset::Medium);
    ASSERT_EQ(stats.effectPlans.size(), static_cast<size_t>(2));
    EXPECT_EQ(stats.effectPlans[0].effectName, "Bloom");
    EXPECT_TRUE(stats.effectPlans[0].requested);
    EXPECT_FALSE(stats.effectPlans[0].supported);
    EXPECT_FALSE(stats.effectPlans[0].enabled);
    EXPECT_FALSE(stats.effectPlans[0].scheduled);
    EXPECT_FALSE(stats.effectPlans[0].pipelineReady);
    EXPECT_FALSE(stats.effectPlans[0].pipelineReadinessReason.empty());
    EXPECT_FALSE(stats.effectPlans[0].skippedReason.empty());
    EXPECT_EQ(stats.effectPlans[0].reason, stats.effectPlans[0].skippedReason);
    EXPECT_EQ(stats.effectPlans[1].effectName, "ToneMapping");
    EXPECT_TRUE(stats.effectPlans[1].requested);
    EXPECT_FALSE(stats.effectPlans[1].supported);
    EXPECT_FALSE(stats.effectPlans[1].enabled);
    EXPECT_FALSE(stats.effectPlans[1].scheduled);
    EXPECT_FALSE(stats.effectPlans[1].pipelineReady);
    EXPECT_FALSE(stats.effectPlans[1].pipelineReadinessReason.empty());
    EXPECT_FALSE(stats.effectPlans[1].skippedReason.empty());
    EXPECT_EQ(stats.effectPlans[1].reason, stats.effectPlans[1].skippedReason);
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

TEST_F(RenderPassValidationFixture, ObjectVelocityPassDrawsMaskedItemsWithMaterialSet)
{
    ASSERT_NO_FATAL_FAILURE(Initialize());

    RenderObject& object = scene.GetMutableObject(0);
    object.previousWorldMatrix = Mat4Identity();
    object.previousWorldMatrix[3][0] = -1.0f;
    object.previousWorldMatrixValid = 1;

    Resource::MaterialResource materialResource;
    materialResource.SetId(601);
    materialResource.SetName("MaskedVelocityMaterial");
    materialResource.SetMaterialData(std::make_shared<Material>());

    RenderDrawItem maskedItem = MakeDrawItem(MaterialRenderMode::Masked);
    maskedItem.materialResource = &materialResource;
    std::vector<RenderDrawItem> opaqueItems;
    std::vector<RenderDrawItem> maskedItems = {maskedItem};

    RenderGraph graph;
    graph.SetDevice(&device);

    RHITextureDesc velocityDesc = RHITextureDesc::RenderTarget(64, 64, RHIFormat::RG16_FLOAT);
    velocityDesc.debugName = "GraphVelocityForObjectVelocityPass";
    view.velocityTarget = graph.CreateTexture(velocityDesc);
    graph.SetExportState(view.velocityTarget, RHIResourceState::RenderTarget);

    RHITextureDesc depthDesc = RHITextureDesc::DepthStencil(64, 64, PipelineCache::GetDefaultDepthStencilFormat());
    depthDesc.debugName = "GraphDepthForObjectVelocityPass";
    view.depthTarget = graph.CreateTexture(depthDesc);
    graph.SetExportState(view.depthTarget, RHIResourceState::DepthRead);

    view.renderGraph = &graph;
    view.viewCache = &viewCache;
    view.viewportWidth = 64;
    view.viewportHeight = 64;
    view.previousViewProjectionMatrix = Mat4Identity();
    view.previousViewProjectionValid = 1;
    view.resetTemporalHistory = false;

    ObjectVelocityPass pass;
    pass.SetResources(&gpuResources, &pipelineCache, &viewCache, &materialSystem);
    pass.SetRenderScene(&scene, &opaqueItems, &maskedItems);
    pass.SetEnabled(true);

    ASSERT_TRUE(pass.IsSupported()) << pass.GetUnsupportedReason();

    pass.AddToGraph(graph, view);
    graph.Compile();
    EXPECT_TRUE(graph.GetCompileStats().compileValid);
    EXPECT_EQ(graph.GetCompileStats().totalPasses, 1u);

    RecordingCommandContext ctx;
    graph.Execute(ctx);

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

TEST_F(RenderPassValidationFixture, DepthPrepassConsumesGPUDrivenMultiMeshIndirectStreams)
{
    ASSERT_NO_FATAL_FAILURE(Initialize());

    auto secondMeshResource = CreateMeshResource(402);
    gpuResources.UploadImmediate(secondMeshResource.get());
    ASSERT_TRUE(gpuResources.IsGPUReady(secondMeshResource->GetId()));

    scene.GetMutableObject(0).bounds = meshResource->GetBounds();
    RenderObject secondObject = MakeRenderObject(*secondMeshResource);
    secondObject.bounds = secondMeshResource->GetBounds();
    scene.AddObject(secondObject);

    RenderDrawItem firstItem = MakeDrawItem(MaterialRenderMode::Opaque);
    RenderDrawItem secondItem = firstItem;
    secondItem.objectIndex = 1;
    secondItem.meshId = secondMeshResource->GetId();
    std::vector<RenderDrawItem> opaqueItems = {firstItem, secondItem};
    std::vector<RenderDrawItem> maskedItems;

    MeshGPUBuffers firstBuffers = gpuResources.GetMeshBuffers(meshResource->GetId());
    ASSERT_TRUE(firstBuffers.IsValid());
    ASSERT_FALSE(firstBuffers.submeshes.empty());
    MeshGPUBuffers secondBuffers = gpuResources.GetMeshBuffers(secondMeshResource->GetId());
    ASSERT_TRUE(secondBuffers.IsValid());
    ASSERT_FALSE(secondBuffers.submeshes.empty());

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
    pass.SetResources(&gpuResources, &pipelineCache);
    pass.SetRenderScene(&scene, &opaqueItems, &maskedItems);
    pass.SetDepthTarget(depthView.Get());
    pass.SetGPUDrivenCullingSource(&culling);

    RecordingCommandContext ctx;
    pass.Execute(ctx, view);

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

TEST_F(RenderPassValidationFixture, OpaquePassConsumesGPUDrivenMaterialGroupedIndirectStreams)
{
    ASSERT_NO_FATAL_FAILURE(Initialize());

    scene.GetMutableObject(0).bounds = meshResource->GetBounds();

    RenderDrawItem opaqueItem = MakeDrawItem(MaterialRenderMode::Opaque);
    RenderDrawItem maskedItem = MakeDrawItem(MaterialRenderMode::Masked);
    std::vector<RenderDrawItem> opaqueItems = {opaqueItem};
    std::vector<RenderDrawItem> maskedItems = {maskedItem};

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
        opaqueItem.materialId,
        MaterialPipelineVariant::Opaque,
        opaqueItem.materialResource));
    EXPECT_NE(RVX_INVALID_INDEX, culling.AddDrawItemInstance(scene, opaqueItem, drawDesc, 0));
    culling.EndDrawGroup();

    ASSERT_EQ(1u, culling.BeginDrawGroup(
        meshResource->GetId(),
        maskedItem.materialId,
        MaterialPipelineVariant::Masked,
        maskedItem.materialResource));
    EXPECT_NE(RVX_INVALID_INDEX, culling.AddDrawItemInstance(scene, maskedItem, drawDesc, 1));
    culling.EndDrawGroup();

    culling.EndFrame();
    culling.CullCpuFallback(view.viewMatrix, view.projectionMatrix);
    EXPECT_EQ(2u, culling.GetDrawCount());
    ASSERT_EQ(2u, culling.GetDrawGroups().size());

    OpaquePass pass;
    pass.SetResources(&gpuResources, &pipelineCache, &materialSystem);
    pass.SetRenderScene(&scene, &opaqueItems, &maskedItems);
    pass.SetRenderTargets(colorView.Get(), nullptr);
    pass.SetGPUDrivenCullingSource(&culling);

    RecordingCommandContext ctx;
    pass.Execute(ctx, view);

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
    EXPECT_TRUE(stats.gpuDrivenEligible);
    EXPECT_EQ(0u, stats.directDrawCount);
    EXPECT_EQ(0u, stats.indirectBatchCount);
    EXPECT_EQ(2u, stats.gpuDrivenIndirectBatchCount);
    EXPECT_EQ(2u, stats.gpuDrivenIndirectDrawCount);
}

TEST_F(RenderPassValidationFixture, OpaquePassBatchesSameObjectSubmeshesWithIndirectDraw)
{
    ASSERT_NO_FATAL_FAILURE(Initialize());

    auto twoSubmeshResource = CreateTwoSubmeshMeshResource(1401);
    gpuResources.UploadImmediate(twoSubmeshResource.get());
    ASSERT_TRUE(gpuResources.IsGPUReady(twoSubmeshResource->GetId()));

    scene.Clear();
    scene.AddObject(MakeRenderObject(*twoSubmeshResource));

    RenderDrawItem firstItem = MakeDrawItem(MaterialRenderMode::Opaque);
    firstItem.meshId = twoSubmeshResource->GetId();
    firstItem.submeshIndex = 0;

    RenderDrawItem secondItem = firstItem;
    secondItem.submeshIndex = 1;

    std::vector<RenderDrawItem> opaqueItems = {firstItem, secondItem};
    std::vector<RenderDrawItem> maskedItems;

    view.viewCache = &viewCache;
    view.viewportWidth = 64;
    view.viewportHeight = 64;

    OpaquePass pass;
    pass.OnAdd(&device);
    pass.SetResources(&gpuResources, &pipelineCache, &materialSystem);
    pass.SetRenderScene(&scene, &opaqueItems, &maskedItems);
    pass.SetRenderTargets(colorView.Get(), nullptr);

    RecordingCommandContext ctx;
    pass.Execute(ctx, view);

    EXPECT_EQ(0u, ctx.drawIndexedCount);
    EXPECT_EQ(1u, ctx.drawIndexedIndirectCount);
    EXPECT_EQ(0u, ctx.lastIndirectOffset);
    EXPECT_EQ(2u, ctx.lastIndirectDrawCount);
    EXPECT_EQ(sizeof(IndirectDrawIndexedCommand), ctx.lastIndirectStride);
    ASSERT_NE(ctx.lastIndirectBuffer, nullptr);

    const auto* indirectBuffer = dynamic_cast<const FakeBuffer*>(ctx.lastIndirectBuffer);
    ASSERT_NE(indirectBuffer, nullptr);
    ASSERT_GE(indirectBuffer->GetStorage().size(), sizeof(IndirectDrawIndexedCommand) * 2);

    std::array<IndirectDrawIndexedCommand, 2> commands;
    std::memcpy(commands.data(), indirectBuffer->GetStorage().data(), sizeof(commands));
    EXPECT_EQ(3u, commands[0].indexCount);
    EXPECT_EQ(1u, commands[0].instanceCount);
    EXPECT_EQ(0u, commands[0].firstIndex);
    EXPECT_EQ(0, commands[0].vertexOffset);
    EXPECT_EQ(0u, commands[0].firstInstance);
    EXPECT_EQ(3u, commands[1].indexCount);
    EXPECT_EQ(1u, commands[1].instanceCount);
    EXPECT_EQ(3u, commands[1].firstIndex);
    EXPECT_EQ(0, commands[1].vertexOffset);
    EXPECT_EQ(0u, commands[1].firstInstance);

    const OpaquePassDrawStats& stats = pass.GetDrawStats();
    EXPECT_EQ(0u, stats.directDrawCount);
    EXPECT_EQ(1u, stats.indirectBatchCount);
    EXPECT_EQ(2u, stats.indirectDrawCount);
}

TEST_F(RenderPassValidationFixture, OpaquePassDoesNotIndirectBatchResolvedMaterialMismatch)
{
    ASSERT_NO_FATAL_FAILURE(Initialize());

    auto twoSubmeshResource = CreateTwoSubmeshMeshResource(1402);
    gpuResources.UploadImmediate(twoSubmeshResource.get());
    ASSERT_TRUE(gpuResources.IsGPUReady(twoSubmeshResource->GetId()));

    Resource::MaterialResource firstMaterial;
    firstMaterial.SetId(701);
    firstMaterial.SetName("FirstSubmeshMaterial");
    firstMaterial.SetMaterialData(std::make_shared<Material>());

    Resource::MaterialResource secondMaterial;
    secondMaterial.SetId(702);
    secondMaterial.SetName("SecondSubmeshMaterial");
    secondMaterial.SetMaterialData(std::make_shared<Material>());

    RenderObject object = MakeRenderObject(*twoSubmeshResource);
    object.materialResources = {&firstMaterial, &secondMaterial};
    object.materialIds = {firstMaterial.GetId(), secondMaterial.GetId()};

    scene.Clear();
    scene.AddObject(object);

    RenderDrawItem firstItem = MakeDrawItem(MaterialRenderMode::Opaque);
    firstItem.meshId = twoSubmeshResource->GetId();
    firstItem.submeshIndex = 0;
    firstItem.materialId = 7000;
    firstItem.materialResource = nullptr;

    RenderDrawItem secondItem = firstItem;
    secondItem.submeshIndex = 1;

    std::vector<RenderDrawItem> opaqueItems = {firstItem, secondItem};
    std::vector<RenderDrawItem> maskedItems;

    view.viewCache = &viewCache;
    view.viewportWidth = 64;
    view.viewportHeight = 64;

    OpaquePass pass;
    pass.OnAdd(&device);
    pass.SetResources(&gpuResources, &pipelineCache, &materialSystem);
    pass.SetRenderScene(&scene, &opaqueItems, &maskedItems);
    pass.SetRenderTargets(colorView.Get(), nullptr);

    RecordingCommandContext ctx;
    pass.Execute(ctx, view);

    EXPECT_EQ(2u, ctx.drawIndexedCount);
    EXPECT_EQ(0u, ctx.drawIndexedIndirectCount);

    const OpaquePassDrawStats& stats = pass.GetDrawStats();
    EXPECT_EQ(2u, stats.directDrawCount);
    EXPECT_EQ(0u, stats.indirectBatchCount);
    EXPECT_EQ(0u, stats.indirectDrawCount);
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
    EXPECT_NE(opaquePass.find("PrepareMaterialBinding("), std::string::npos);
    EXPECT_NE(opaquePass.find("item.material, view.viewCache, materialOptions"),
              std::string::npos);
    EXPECT_NE(opaquePass.find("materialResource, view.viewCache, materialOptions"),
              std::string::npos);

    EXPECT_NE(transparentPass.find("MaterialBindingOptions materialOptions;"), std::string::npos);
    EXPECT_NE(transparentPass.find("materialOptions.allowNormalMap = buffers.HasNormalMapTangentBasis()"),
              std::string::npos);
    EXPECT_NE(transparentPass.find("PrepareMaterialBinding("), std::string::npos);
    EXPECT_NE(transparentPass.find("item.material, view.viewCache, materialOptions"),
              std::string::npos);
    EXPECT_NE(transparentPass.find("materialResource, view.viewCache, materialOptions"),
              std::string::npos);
}

TEST_F(RenderPassValidationFixture, OpaqueAndTransparentPassBindFrameLightResources)
{
    ASSERT_NO_FATAL_FAILURE(Initialize());

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
    EXPECT_NE(opaquePass.find("obj.receivesShadow,"), std::string::npos);
    EXPECT_NE(opaquePass.find("m_pipelineCache->UpdateFrameLightResources(lightResources);"), std::string::npos);

    EXPECT_NE(transparentPass.find("#include \"Render/Lighting/LightManager.h\""), std::string::npos);
    EXPECT_NE(transparentPass.find("#include \"Render/Lighting/ClusteredLighting.h\""), std::string::npos);
    EXPECT_NE(transparentPass.find("lightResources.lightConstantsBuffer = m_lightManager->GetLightConstantsBuffer();"),
              std::string::npos);
    EXPECT_NE(transparentPass.find("lightResources.pointLightsBuffer = m_lightManager->GetPointLightsBuffer();"),
              std::string::npos);
    EXPECT_NE(transparentPass.find("lightResources.spotLightsBuffer = m_lightManager->GetSpotLightsBuffer();"),
              std::string::npos);
    EXPECT_NE(transparentPass.find("lightResources.clusterConstantsBuffer = m_clusteredLighting->GetClusterConstantsBuffer();"),
              std::string::npos);
    EXPECT_NE(transparentPass.find("lightResources.clusterBuffer = m_clusteredLighting->GetClusterBuffer();"),
              std::string::npos);
    EXPECT_NE(transparentPass.find("lightResources.clusterLightIndexBuffer = m_clusteredLighting->GetLightIndexBuffer();"),
              std::string::npos);
    EXPECT_NE(transparentPass.find("obj.receivesShadow,"), std::string::npos);

    const size_t shadowUpdate = transparentPass.find("m_pipelineCache->UpdateDirectionalShadowFrameResources({});");
    const size_t lightUpdate = transparentPass.find("m_pipelineCache->UpdateFrameLightResources(lightResources);");
    ASSERT_NE(shadowUpdate, std::string::npos);
    ASSERT_NE(lightUpdate, std::string::npos);
    EXPECT_LT(shadowUpdate, lightUpdate);
}

TEST_F(RenderPassValidationFixture, OpaquePassReportsShadowReceiverOptOutDrawItems)
{
    ASSERT_NO_FATAL_FAILURE(Initialize());

    RenderObject& firstObject = scene.GetMutableObject(0);
    firstObject.receivesShadow = true;

    RenderObject secondObject = MakeRenderObject(*meshResource);
    secondObject.receivesShadow = false;
    scene.AddObject(secondObject);

    RenderDrawItem firstItem = MakeDrawItem(MaterialRenderMode::Opaque);
    RenderDrawItem secondItem = MakeDrawItem(MaterialRenderMode::Masked);
    secondItem.objectIndex = 1;

    std::vector<RenderDrawItem> opaqueItems = {firstItem};
    std::vector<RenderDrawItem> maskedItems = {secondItem};

    RenderGraph graph;
    graph.SetDevice(&device);
    view.colorTarget = graph.ImportTexture(colorTexture.Get(), RHIResourceState::RenderTarget);

    OpaquePass pass;
    pass.OnAdd(&device);
    pass.SetResources(&gpuResources, &pipelineCache, &materialSystem);
    pass.SetRenderScene(&scene, &opaqueItems, &maskedItems);
    pass.AddToGraph(graph, view);

    const OpaquePassShadowStats& stats = pass.GetShadowStats();
    EXPECT_EQ(stats.receiverCandidateDrawItemCount, 2u);
    EXPECT_EQ(stats.shadowReceivingDrawItemCount, 1u);
    EXPECT_EQ(stats.shadowReceiverOptOutDrawItemCount, 1u);
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
    shadowConfig.cascadeBlendRatio = 0.1f;

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
    shadowConfig.numCascades = 2;
    shadowConfig.shadowMapSize = 64;
    shadowConfig.cascadeBlendRatio = 0.1f;

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
