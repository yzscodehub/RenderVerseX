#include "Common/DeterministicShaderCompiler.h"
#include "Common/RenderGraphValidationAccess.h"
#include "Core/Log.h"
#include "Core/Serialization/Serialization.h"
#include "Render/Debug/GPUProfiler.h"
#include "Render/Material/MaterialBinder.h"
#include "Render/Material/MaterialTemplate.h"
#include "Render/Passes/ParticleFeaturePass.h"
#include "Render/Passes/RayTracedShadowPass.h"
#include "Render/Passes/SkyboxPass.h"
#include "Render/PostProcess/Bloom.h"
#include "Render/PostProcess/ChromaticAberration.h"
#include "Render/PostProcess/ColorGrading.h"
#include "Render/PostProcess/DOF.h"
#include "Render/PostProcess/FXAA.h"
#include "Render/PostProcess/FilmGrain.h"
#include "Render/PostProcess/MotionBlur.h"
#include "Render/PostProcess/SSAO.h"
#include "Render/PostProcess/SSR.h"
#include "Render/PostProcess/TAA.h"
#include "Render/PostProcess/ToneMapping.h"
#include "Render/PostProcess/Vignette.h"
#include "Render/PostProcess/VolumetricLighting.h"
#include "Render/Graph/RenderGraph.h"
#include "Render/Renderer/ViewData.h"
#include "Render/Sky/AtmosphericScattering.h"
#include "Resource/Importer/GLTFImporter.h"
#include "Resource/CookManifest.h"
#include "Resource/Loader/TextureLoader.h"
#include "Resource/Types/MaterialResource.h"
#include "Resource/Types/MeshResource.h"
#include "Resource/Types/ModelResource.h"
#include "Resource/Types/ShaderResource.h"
#include "RHI/RHICommandContext.h"
#include "RHI/RHI.h"
#include "Geometry/Asset/Mesh.h"
#include "Terrain/Heightmap.h"
#include "Terrain/TerrainLOD.h"
#include "Terrain/TerrainMaterial.h"
#include "Particle/ParticleSystem.h"
#include "Particle/ParticleSystemInstance.h"
#include "Tools/AssetDatabase.h"
#include "Tools/AssetPipeline.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace
{
    namespace fs = std::filesystem;

    class NullDevice : public RVX::IRHIDevice
    {
    public:
        RVX::RHIBufferRef CreateBuffer(const RVX::RHIBufferDesc&) override { return {}; }
        RVX::RHITextureRef CreateTexture(const RVX::RHITextureDesc&) override { return {}; }
        RVX::RHITextureViewRef CreateTextureView(RVX::RHITexture*, const RVX::RHITextureViewDesc&) override { return {}; }
        RVX::RHISamplerRef CreateSampler(const RVX::RHISamplerDesc&) override { return {}; }
        RVX::RHIShaderRef CreateShader(const RVX::RHIShaderDesc&) override { return {}; }
        RVX::RHIHeapRef CreateHeap(const RVX::RHIHeapDesc&) override { return {}; }
        RVX::RHITextureRef CreatePlacedTexture(RVX::RHIHeap*, RVX::uint64, const RVX::RHITextureDesc&) override { return {}; }
        RVX::RHIBufferRef CreatePlacedBuffer(RVX::RHIHeap*, RVX::uint64, const RVX::RHIBufferDesc&) override { return {}; }
        MemoryRequirements GetTextureMemoryRequirements(const RVX::RHITextureDesc&) override { return {}; }
        MemoryRequirements GetBufferMemoryRequirements(const RVX::RHIBufferDesc&) override { return {}; }
        RVX::RHIDescriptorSetLayoutRef CreateDescriptorSetLayout(const RVX::RHIDescriptorSetLayoutDesc&) override { return {}; }
        RVX::RHIPipelineLayoutRef CreatePipelineLayout(const RVX::RHIPipelineLayoutDesc&) override { return {}; }
        RVX::RHIPipelineRef CreateGraphicsPipeline(const RVX::RHIGraphicsPipelineDesc&) override { return {}; }
        RVX::RHIPipelineRef CreateComputePipeline(const RVX::RHIComputePipelineDesc&) override { return {}; }
        RVX::RHIDescriptorSetRef CreateDescriptorSet(const RVX::RHIDescriptorSetDesc&) override { return {}; }
        RVX::RHIQueryPoolRef CreateQueryPool(const RVX::RHIQueryPoolDesc&) override { return {}; }
        RVX::RHICommandContextRef CreateCommandContext(RVX::RHICommandQueueType) override { return {}; }
        RVX::uint64 SubmitCommandContext(RVX::RHICommandContext*, RVX::RHIFence*) override { return 0; }
        RVX::uint64 SubmitCommandContexts(std::span<RVX::RHICommandContext* const>, RVX::RHIFence*) override { return 0; }
        RVX::RHISwapChainRef CreateSwapChain(const RVX::RHISwapChainDesc&) override { return {}; }
        RVX::RHIFenceRef CreateFence(RVX::uint64) override { return {}; }
        void WaitForFence(RVX::RHIFence*, RVX::uint64) override {}
        void WaitIdle() override {}
        void BeginFrame() override {}
        void EndFrame() override {}
        RVX::uint32 GetCurrentFrameIndex() const override { return 0; }
        RVX::RHIStagingBufferRef CreateStagingBuffer(const RVX::RHIStagingBufferDesc&) override { return {}; }
        RVX::RHIRingBufferRef CreateRingBuffer(const RVX::RHIRingBufferDesc&) override { return {}; }
        RVX::RHIMemoryStats GetMemoryStats() const override { return {}; }
        void BeginResourceGroup(const char*) override {}
        void EndResourceGroup() override {}
        const RVX::RHICapabilities& GetCapabilities() const override { return m_capabilities; }
        RVX::RHIBackendType GetBackendType() const override { return RVX::RHIBackendType::None; }

    private:
        RVX::RHICapabilities m_capabilities;
    };

    class NoOpCommandContext final : public RVX::RHICommandContext
    {
    public:
        void Begin() override {}
        void End() override {}
        void Reset() override {}
        void BeginEvent(const char*, RVX::uint32 = 0) override {}
        void EndEvent() override {}
        void SetMarker(const char*, RVX::uint32 = 0) override {}
        void BufferBarrier(const RVX::RHIBufferBarrier&) override {}
        void TextureBarrier(const RVX::RHITextureBarrier&) override {}
        void Barriers(std::span<const RVX::RHIBufferBarrier>,
                      std::span<const RVX::RHITextureBarrier>) override {}
        void BeginBarrier(const RVX::RHIBufferBarrier&) override {}
        void BeginBarrier(const RVX::RHITextureBarrier&) override {}
        void EndBarrier(const RVX::RHIBufferBarrier&) override {}
        void EndBarrier(const RVX::RHITextureBarrier&) override {}
        void BeginRenderPass(const RVX::RHIRenderPassDesc&) override {}
        void EndRenderPass() override {}
        void SetPipeline(RVX::RHIPipeline*) override {}
        void SetVertexBuffer(RVX::uint32, RVX::RHIBuffer*, RVX::uint64 = 0) override {}
        void SetVertexBuffers(RVX::uint32,
                              std::span<RVX::RHIBuffer* const>,
                              std::span<const RVX::uint64> = {}) override {}
        void SetIndexBuffer(RVX::RHIBuffer*, RVX::RHIFormat, RVX::uint64 = 0) override {}
        void SetDescriptorSet(RVX::uint32, RVX::RHIDescriptorSet*, std::span<const RVX::uint32> = {}) override {}
        void SetPushConstants(const void*, RVX::uint32, RVX::uint32 = 0) override {}
        void SetViewport(const RVX::RHIViewport&) override {}
        void SetViewports(std::span<const RVX::RHIViewport>) override {}
        void SetScissor(const RVX::RHIRect&) override {}
        void SetScissors(std::span<const RVX::RHIRect>) override {}
        void Draw(RVX::uint32, RVX::uint32 = 1, RVX::uint32 = 0, RVX::uint32 = 0) override { drawCount++; }
        void DrawIndexed(RVX::uint32,
                         RVX::uint32 = 1,
                         RVX::uint32 = 0,
                         RVX::int32 = 0,
                         RVX::uint32 = 0) override { drawIndexedCount++; }
        void DrawIndirect(RVX::RHIBuffer*, RVX::uint64, RVX::uint32, RVX::uint32) override
        {
            drawIndirectCount++;
        }
        void DrawIndexedIndirect(RVX::RHIBuffer*, RVX::uint64, RVX::uint32, RVX::uint32) override
        {
            drawIndexedIndirectCount++;
        }
        void Dispatch(RVX::uint32, RVX::uint32, RVX::uint32) override {}
        void DispatchIndirect(RVX::RHIBuffer*, RVX::uint64) override {}
        void CopyBuffer(RVX::RHIBuffer*, RVX::RHIBuffer*, RVX::uint64, RVX::uint64, RVX::uint64) override {}
        void CopyTexture(RVX::RHITexture*, RVX::RHITexture*, const RVX::RHITextureCopyDesc& = {}) override {}
        void CopyBufferToTexture(RVX::RHIBuffer*, RVX::RHITexture*, const RVX::RHIBufferTextureCopyDesc&) override {}
        void CopyTextureToBuffer(RVX::RHITexture*, RVX::RHIBuffer*, const RVX::RHIBufferTextureCopyDesc&) override {}
        void BeginQuery(RVX::RHIQueryPool*, RVX::uint32) override {}
        void EndQuery(RVX::RHIQueryPool*, RVX::uint32) override {}
        void WriteTimestamp(RVX::RHIQueryPool*, RVX::uint32) override {}
        void ResolveQueries(RVX::RHIQueryPool*, RVX::uint32, RVX::uint32, RVX::RHIBuffer*, RVX::uint64) override {}
        void ResetQueries(RVX::RHIQueryPool*, RVX::uint32, RVX::uint32) override {}
        void SetStencilReference(RVX::uint32) override {}
        void SetBlendConstants(const float[4]) override {}
        void SetDepthBias(float, float, float = 0.0f) override {}
        void SetDepthBounds(float, float) override {}
        void SetStencilReferenceSeparate(RVX::uint32, RVX::uint32) override {}
        void SetLineWidth(float) override {}
        void SignalFence(RVX::RHIFence*, RVX::uint64) override {}
        void WaitFence(RVX::RHIFence*, RVX::uint64) override {}

        RVX::uint32 drawCount = 0;
        RVX::uint32 drawIndexedCount = 0;
        RVX::uint32 drawIndirectCount = 0;
        RVX::uint32 drawIndexedIndirectCount = 0;
    };

    class FakeBuffer final : public RVX::RHIBuffer
    {
    public:
        explicit FakeBuffer(RVX::RHIBufferUsage usage = RVX::RHIBufferUsage::Vertex,
                            RVX::uint64 size = 256)
            : m_usage(usage)
            , m_size(size)
        {
        }

        RVX::uint64 GetSize() const override { return m_size; }
        RVX::RHIBufferUsage GetUsage() const override { return m_usage; }
        RVX::RHIMemoryType GetMemoryType() const override { return RVX::RHIMemoryType::Default; }
        RVX::uint32 GetStride() const override { return sizeof(float) * 3; }
        void* Map() override { return nullptr; }
        void Unmap() override {}

    private:
        RVX::RHIBufferUsage m_usage = RVX::RHIBufferUsage::Vertex;
        RVX::uint64 m_size = 256;
    };

    class TerrainNullMapBufferDevice final : public NullDevice
    {
    public:
        RVX::RHIBufferRef CreateBuffer(const RVX::RHIBufferDesc& desc) override
        {
            return RVX::MakeRef<FakeBuffer>(desc.usage, desc.size);
        }
    };

    class FakeAccelerationStructure final : public RVX::RHIAccelerationStructure
    {
    public:
        explicit FakeAccelerationStructure(RVX::RHIAccelerationStructureType type,
                                           RVX::uint64 gpuVirtualAddress = 0x1000)
            : m_type(type)
            , m_gpuVirtualAddress(gpuVirtualAddress)
        {
        }

        RVX::RHIAccelerationStructureType GetType() const override { return m_type; }
        RVX::uint64 GetSize() const override { return 4096; }
        RVX::uint64 GetGPUVirtualAddress() const override { return m_gpuVirtualAddress; }

    private:
        RVX::RHIAccelerationStructureType m_type = RVX::RHIAccelerationStructureType::BottomLevel;
        RVX::uint64 m_gpuVirtualAddress = 0x1000;
    };

    class FakeShader final : public RVX::RHIShader
    {
    public:
        explicit FakeShader(RVX::RHIShaderStage stage)
            : m_stage(stage)
        {
        }

        RVX::RHIShaderStage GetStage() const override { return m_stage; }
        const std::vector<RVX::uint8>& GetBytecode() const override { return m_bytecode; }

    private:
        RVX::RHIShaderStage m_stage = RVX::RHIShaderStage::None;
        std::vector<RVX::uint8> m_bytecode = {0, 1, 2, 3};
    };

    class FakePipelineLayout final : public RVX::RHIPipelineLayout
    {
    };

    class FakePipeline final : public RVX::RHIPipeline
    {
    public:
        explicit FakePipeline(bool rayTracing)
            : m_rayTracing(rayTracing)
        {
            if (m_rayTracing)
            {
                m_shaderGroupStages = {
                    RVX::RHIShaderStage::RayGeneration,
                    RVX::RHIShaderStage::Miss,
                    RVX::RHIShaderStage::None,
                    RVX::RHIShaderStage::Callable
                };
                m_shaderGroupIsHitGroup = {false, false, true, false};
            }
        }

        bool IsCompute() const override { return false; }
        bool IsRayTracing() const override { return m_rayTracing; }
        RVX::uint32 GetRayTracingShaderGroupCount() const override
        {
            return static_cast<RVX::uint32>(m_shaderGroupStages.size());
        }
        RVX::RHIShaderStage GetRayTracingShaderGroupStage(RVX::uint32 shaderGroupIndex) const override
        {
            return shaderGroupIndex < m_shaderGroupStages.size()
                ? m_shaderGroupStages[shaderGroupIndex]
                : RVX::RHIShaderStage::None;
        }
        bool IsRayTracingHitGroup(RVX::uint32 shaderGroupIndex) const override
        {
            return shaderGroupIndex < m_shaderGroupIsHitGroup.size() && m_shaderGroupIsHitGroup[shaderGroupIndex];
        }

    private:
        bool m_rayTracing = false;
        std::vector<RVX::RHIShaderStage> m_shaderGroupStages;
        std::vector<bool> m_shaderGroupIsHitGroup;
    };

    class FakeShaderTable final : public RVX::RHIShaderTable
    {
    public:
        explicit FakeShaderTable(RVX::RHIPipeline* rayTracingPipeline = nullptr,
                                 RVX::uint32 rayGenerationRecordCount = 1,
                                 RVX::uint32 missRecordCount = 1,
                                 RVX::uint32 hitGroupRecordCount = 1,
                                 RVX::uint32 callableRecordCount = 0)
            : m_pipeline(rayTracingPipeline)
            , m_rayGenerationRecordCount(rayGenerationRecordCount)
            , m_missRecordCount(missRecordCount)
            , m_hitGroupRecordCount(hitGroupRecordCount)
            , m_callableRecordCount(callableRecordCount)
        {
        }

        RVX::uint32 GetRayGenerationRecordCount() const override { return m_rayGenerationRecordCount; }
        RVX::uint32 GetMissRecordCount() const override { return m_missRecordCount; }
        RVX::uint32 GetHitGroupRecordCount() const override { return m_hitGroupRecordCount; }
        RVX::uint32 GetCallableRecordCount() const override { return m_callableRecordCount; }
        RVX::RHIPipeline* GetRayTracingPipeline() const override { return m_pipeline; }

    private:
        RVX::RHIPipeline* m_pipeline = nullptr;
        RVX::uint32 m_rayGenerationRecordCount = 1;
        RVX::uint32 m_missRecordCount = 1;
        RVX::uint32 m_hitGroupRecordCount = 1;
        RVX::uint32 m_callableRecordCount = 0;
    };

    class FakeDescriptorSetLayout final : public RVX::RHIDescriptorSetLayout
    {
    public:
        explicit FakeDescriptorSetLayout(std::vector<RVX::RHIBindingLayoutEntry> entries)
            : m_entries(std::move(entries))
        {
        }

        const std::vector<RVX::RHIBindingLayoutEntry>& GetEntries() const override { return m_entries; }

    private:
        std::vector<RVX::RHIBindingLayoutEntry> m_entries;
    };

    fs::path MakeTempDir(const char* name)
    {
        const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
        fs::path dir = fs::temp_directory_path() / (std::string(name) + "_" + std::to_string(ticks));
        fs::create_directories(dir);
        return dir;
    }

    fs::path FindRepoRoot()
    {
        fs::path cursor = fs::current_path();
        for (uint32_t i = 0; i < 8; ++i)
        {
            if (fs::exists(cursor / "Render" / "Include" / "Render" / "Renderer" / "SceneRenderer.h"))
            {
                return cursor;
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

    void WriteTextFile(const fs::path& path, const std::string& text)
    {
        fs::create_directories(path.parent_path());
        std::ofstream file(path, std::ios::binary);
        file << text;
    }

    void WriteBinaryFile(const fs::path& path, const std::vector<uint8_t>& bytes)
    {
        fs::create_directories(path.parent_path());
        std::ofstream file(path, std::ios::binary);
        file.write(reinterpret_cast<const char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
    }

    std::string ReadBinaryFile(const fs::path& path)
    {
        std::ifstream file(path, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(file)),
                           std::istreambuf_iterator<char>());
    }

    bool ExtractCookedTexturePayload(const std::string& artifact, std::vector<uint8_t>& outPayload)
    {
        constexpr const char* dataMarker = "RVX_TEXTURE_DATA_BEGIN\n";
        constexpr const char* endMarker = "\nRVX_TEXTURE_PREBAKE_END\n";
        const size_t payloadBegin = artifact.find(dataMarker);
        if (payloadBegin == std::string::npos)
        {
            return false;
        }

        const size_t dataOffset = payloadBegin + std::strlen(dataMarker);
        const size_t payloadEnd = artifact.find(endMarker, dataOffset);
        if (payloadEnd == std::string::npos || payloadEnd < dataOffset)
        {
            return false;
        }

        outPayload.assign(artifact.begin() + static_cast<std::ptrdiff_t>(dataOffset),
                          artifact.begin() + static_cast<std::ptrdiff_t>(payloadEnd));
        return true;
    }

    uint64_t ReadBC7Bits(const uint8_t* block, uint32_t& bitOffset, uint32_t bitCount)
    {
        uint64_t value = 0;
        for (uint32_t bit = 0; bit < bitCount; ++bit)
        {
            const uint32_t sourceBit = bitOffset + bit;
            const uint8_t byte = block[sourceBit / 8u];
            const uint8_t mask = static_cast<uint8_t>(1u << (sourceBit % 8u));
            if ((byte & mask) != 0u)
            {
                value |= 1ull << bit;
            }
        }
        bitOffset += bitCount;
        return value;
    }

    uint8_t ExpandBC7Endpoint7(uint8_t endpoint, uint8_t pBit)
    {
        return static_cast<uint8_t>((endpoint << 1u) | (pBit & 1u));
    }

    uint8_t InterpolateBC7Mode6(uint8_t endpoint0, uint8_t endpoint1, uint32_t index)
    {
        static constexpr std::array<uint8_t, 16> weights = {
            0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64
        };
        const uint32_t weight = weights[index & 0xFu];
        return static_cast<uint8_t>(((64u - weight) * endpoint0 + weight * endpoint1 + 32u) >> 6u);
    }

    bool DecodeBC7Mode6Block(const uint8_t* block,
                             std::array<std::array<uint8_t, 4>, 16>& outPixels)
    {
        uint32_t bitOffset = 0;
        const uint64_t modeSelector = ReadBC7Bits(block, bitOffset, 7u);
        if (modeSelector != (1u << 6u))
        {
            return false;
        }

        const uint8_t r0 = static_cast<uint8_t>(ReadBC7Bits(block, bitOffset, 7u));
        const uint8_t r1 = static_cast<uint8_t>(ReadBC7Bits(block, bitOffset, 7u));
        const uint8_t g0 = static_cast<uint8_t>(ReadBC7Bits(block, bitOffset, 7u));
        const uint8_t g1 = static_cast<uint8_t>(ReadBC7Bits(block, bitOffset, 7u));
        const uint8_t b0 = static_cast<uint8_t>(ReadBC7Bits(block, bitOffset, 7u));
        const uint8_t b1 = static_cast<uint8_t>(ReadBC7Bits(block, bitOffset, 7u));
        const uint8_t a0 = static_cast<uint8_t>(ReadBC7Bits(block, bitOffset, 7u));
        const uint8_t a1 = static_cast<uint8_t>(ReadBC7Bits(block, bitOffset, 7u));
        const uint8_t p0 = static_cast<uint8_t>(ReadBC7Bits(block, bitOffset, 1u));
        const uint8_t p1 = static_cast<uint8_t>(ReadBC7Bits(block, bitOffset, 1u));

        const std::array<uint8_t, 4> endpoint0 = {
            ExpandBC7Endpoint7(r0, p0),
            ExpandBC7Endpoint7(g0, p0),
            ExpandBC7Endpoint7(b0, p0),
            ExpandBC7Endpoint7(a0, p0)
        };
        const std::array<uint8_t, 4> endpoint1 = {
            ExpandBC7Endpoint7(r1, p1),
            ExpandBC7Endpoint7(g1, p1),
            ExpandBC7Endpoint7(b1, p1),
            ExpandBC7Endpoint7(a1, p1)
        };

        std::array<uint8_t, 16> indices{};
        indices[0] = static_cast<uint8_t>(ReadBC7Bits(block, bitOffset, 3u));
        for (size_t pixelIndex = 1; pixelIndex < indices.size(); ++pixelIndex)
        {
            indices[pixelIndex] = static_cast<uint8_t>(ReadBC7Bits(block, bitOffset, 4u));
        }

        for (size_t pixelIndex = 0; pixelIndex < outPixels.size(); ++pixelIndex)
        {
            for (uint32_t channel = 0; channel < 4; ++channel)
            {
                outPixels[pixelIndex][channel] =
                    InterpolateBC7Mode6(endpoint0[channel], endpoint1[channel], indices[pixelIndex]);
            }
        }

        return true;
    }

    std::string QuoteCommandArgument(const fs::path& path)
    {
        std::string value = path.string();
        std::string quoted = "\"";
        for (char ch : value)
        {
            if (ch == '"')
            {
                quoted += "\\\"";
            }
            else
            {
                quoted += ch;
            }
        }
        quoted += "\"";
        return quoted;
    }

    std::string WrapSystemCommand(std::string command)
    {
#if defined(_WIN32)
        return "\"" + command + "\"";
#else
        return command;
#endif
    }

    size_t MipOffset(uint32_t width, uint32_t height, uint32_t mipLevel)
    {
        size_t offset = 0;
        for (uint32_t mip = 0; mip < mipLevel; ++mip)
        {
            offset += static_cast<size_t>(std::max(1u, width >> mip)) *
                      std::max(1u, height >> mip) *
                      4u;
        }
        return offset;
    }

    void WriteRgbaTga(const fs::path& path, uint16_t width, uint16_t height, const std::vector<uint8_t>& rgba)
    {
        fs::create_directories(path.parent_path());
        std::ofstream file(path, std::ios::binary);

        uint8_t header[18] = {};
        header[2] = 2; // Uncompressed true-color image.
        header[12] = static_cast<uint8_t>(width & 0xFFu);
        header[13] = static_cast<uint8_t>((width >> 8u) & 0xFFu);
        header[14] = static_cast<uint8_t>(height & 0xFFu);
        header[15] = static_cast<uint8_t>((height >> 8u) & 0xFFu);
        header[16] = 32;
        header[17] = 0x20 | 8; // Top-left origin, 8 alpha bits.
        file.write(reinterpret_cast<const char*>(header), sizeof(header));

        for (size_t i = 0; i + 3 < rgba.size(); i += 4)
        {
            const uint8_t bgra[4] = {rgba[i + 2], rgba[i + 1], rgba[i + 0], rgba[i + 3]};
            file.write(reinterpret_cast<const char*>(bgra), sizeof(bgra));
        }
    }

    template <typename T>
    void AppendPod(std::vector<uint8_t>& bytes, const T& value)
    {
        const uint8_t* raw = reinterpret_cast<const uint8_t*>(&value);
        bytes.insert(bytes.end(), raw, raw + sizeof(T));
    }

    void AppendVec2(std::vector<uint8_t>& bytes, float x, float y)
    {
        AppendPod(bytes, x);
        AppendPod(bytes, y);
    }

    void AppendVec3(std::vector<uint8_t>& bytes, float x, float y, float z)
    {
        AppendPod(bytes, x);
        AppendPod(bytes, y);
        AppendPod(bytes, z);
    }

    void WriteMinimalQuadGltf(const fs::path& path)
    {
        constexpr size_t positionOffset = 0;
        constexpr size_t normalOffset = 48;
        constexpr size_t uvOffset = 96;
        constexpr size_t indexOffset = 128;

        std::vector<uint8_t> buffer;
        buffer.reserve(140);

        AppendVec3(buffer, -0.5f, -0.5f, 0.0f);
        AppendVec3(buffer, 0.5f, -0.5f, 0.0f);
        AppendVec3(buffer, 0.5f, 0.5f, 0.0f);
        AppendVec3(buffer, -0.5f, 0.5f, 0.0f);

        for (uint32_t i = 0; i < 4; ++i)
        {
            AppendVec3(buffer, 0.0f, 0.0f, 1.0f);
        }

        AppendVec2(buffer, 0.0f, 0.0f);
        AppendVec2(buffer, 1.0f, 0.0f);
        AppendVec2(buffer, 1.0f, 1.0f);
        AppendVec2(buffer, 0.0f, 1.0f);

        const uint16_t indices[] = {0, 1, 2, 0, 2, 3};
        for (uint16_t index : indices)
        {
            AppendPod(buffer, index);
        }

        const fs::path binPath = path.parent_path() / "Quad.bin";
        WriteBinaryFile(binPath, buffer);

        const std::string json = std::string(R"({
  "asset": { "version": "2.0" },
  "buffers": [
    { "uri": "Quad.bin", "byteLength": )") + std::to_string(buffer.size()) + R"( }
  ],
  "bufferViews": [
    { "buffer": 0, "byteOffset": )" + std::to_string(positionOffset) + R"(, "byteLength": 48, "target": 34962 },
    { "buffer": 0, "byteOffset": )" + std::to_string(normalOffset) + R"(, "byteLength": 48, "target": 34962 },
    { "buffer": 0, "byteOffset": )" + std::to_string(uvOffset) + R"(, "byteLength": 32, "target": 34962 },
    { "buffer": 0, "byteOffset": )" + std::to_string(indexOffset) + R"(, "byteLength": 12, "target": 34963 }
  ],
  "accessors": [
    { "bufferView": 0, "componentType": 5126, "count": 4, "type": "VEC3", "min": [-0.5, -0.5, 0.0], "max": [0.5, 0.5, 0.0] },
    { "bufferView": 1, "componentType": 5126, "count": 4, "type": "VEC3" },
    { "bufferView": 2, "componentType": 5126, "count": 4, "type": "VEC2" },
    { "bufferView": 3, "componentType": 5123, "count": 6, "type": "SCALAR" }
  ],
  "meshes": [
    {
      "name": "CookQuad",
      "primitives": [
        { "attributes": { "POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2 }, "indices": 3, "mode": 4 }
      ]
    }
  ],
  "nodes": [ { "mesh": 0 } ],
  "scenes": [ { "nodes": [0] } ],
  "scene": 0
}
)";

        WriteTextFile(path, json);
    }

    class RenderHonestyValidationFixture : public ::testing::Test
    {
    protected:
        static void SetUpTestSuite()
        {
            RVX::Log::Initialize();
        }

        static void TearDownTestSuite()
        {
            RVX::Log::Shutdown();
        }
    };

    class SuccessWithoutOutputImporter final : public RVX::Tools::IAssetImporter
    {
    public:
        const char* GetName() const override { return "SuccessWithoutOutputImporter"; }
        std::vector<std::string> GetSupportedExtensions() const override { return {".png"}; }
        RVX::Tools::AssetType GetAssetType() const override { return RVX::Tools::AssetType::Texture; }

        RVX::Tools::ImportResult Import(const fs::path&,
                                        const fs::path&,
                                        const void*) override
        {
            RVX::Tools::ImportResult result;
            result.success = true;
            return result;
        }
    };

    class TransactionTestImporter final : public RVX::Tools::IAssetImporter
    {
    public:
        const char* GetName() const override { return "TransactionTestImporter"; }
        std::vector<std::string> GetSupportedExtensions() const override
        {
            return {".ok", ".fail", ".missing", ".vanish"};
        }
        RVX::Tools::AssetType GetAssetType() const override
        {
            return RVX::Tools::AssetType::Texture;
        }

        RVX::Tools::ImportResult Import(const fs::path& sourcePath,
                                        const fs::path& outputPath,
                                        const void*) override
        {
            RVX::Tools::ImportResult result;
            const std::string extension = sourcePath.extension().string();
            if (extension == ".vanish")
            {
                result.success = true;
                return result;
            }

            std::ofstream output(outputPath, std::ios::binary);
            output << "transaction-cooked=" << sourcePath.filename().string();
            output.close();
            if (!output)
            {
                result.error = "Failed to write transaction test artifact";
                return result;
            }

            if (extension == ".fail")
            {
                result.error = "Intentional second importer failure";
                return result;
            }

            result.success = true;
            if (extension == ".missing")
            {
                result.outputPaths.push_back(
                    (outputPath.parent_path() / "declared-but-missing.rva").string());
            }
            return result;
        }
    };

    void ExpectNoCookTransactionResidue(const fs::path& outputRoot)
    {
        const std::string prefix = "." + outputRoot.filename().string() + ".rvx-cook-";
        for (const fs::directory_entry& entry : fs::directory_iterator(outputRoot.parent_path()))
        {
            EXPECT_EQ(entry.path().filename().string().find(prefix), std::string::npos)
                << "stale cook transaction path: " << entry.path();
        }
    }
}

TEST_F(RenderHonestyValidationFixture, JsonArchiveRejectsInvalidJson)
{
    RVX::JsonArchive archive(RVX::ArchiveMode::Read);
    EXPECT_FALSE(archive.Parse("{ invalid json"));
    EXPECT_FALSE(archive.Parse(""));
    EXPECT_FALSE(archive.Parse("{\"value\": 1,}"));
    EXPECT_FALSE(archive.Parse("[1, 2, ]"));
    EXPECT_FALSE(archive.Parse("{\"value\" 1}"));
    EXPECT_FALSE(archive.Parse("{\"value\": 01}"));
    EXPECT_TRUE(archive.Parse("{}"));
    EXPECT_TRUE(archive.Parse("{\"value\": [true, false, null, -1.25e+2]}"));
}

TEST_F(RenderHonestyValidationFixture, JsonArchiveReadPathReportsUnsupportedInsteadOfPretendingSuccess)
{
    RVX::JsonArchive archive(RVX::ArchiveMode::Read);
    EXPECT_FALSE(archive.IsReadSupported());
    ASSERT_TRUE(archive.Parse("{\"enabled\": true, \"count\": 42, \"name\": \"Loaded\"}"));
    EXPECT_FALSE(archive.HasUnsupportedRead());
    EXPECT_NE(archive.GetUnsupportedReason().find("Parse only validates JSON syntax"),
              std::string::npos);

    bool enabled = false;
    RVX::int32 count = 7;
    std::string name = "unchanged";
    archive.Serialize("enabled", enabled);
    archive.Serialize("count", count);
    archive.Serialize("name", name);

    EXPECT_FALSE(enabled);
    EXPECT_EQ(count, 7);
    EXPECT_EQ(name, "unchanged");
    EXPECT_TRUE(archive.HasUnsupportedRead());
    EXPECT_NE(archive.GetUnsupportedReason().find("unsupported"), std::string::npos);
    EXPECT_NE(archive.GetUnsupportedReason().find("name"), std::string::npos);
}

TEST_F(RenderHonestyValidationFixture, PlaceholderAssetImportersFailInsteadOfReportingSuccess)
{
    fs::path dir = MakeTempDir("rvx_asset_importers");
    const fs::path source = dir / "source.png";
    const fs::path output = dir / "source.rva";
    WriteTextFile(source, "not actually a png");

    RVX::Tools::TextureImporter textureImporter;
    RVX::Tools::ImportResult result = textureImporter.Import(source, output);

    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.error.empty());
    EXPECT_TRUE(result.outputPaths.empty());
    EXPECT_FALSE(fs::exists(output));

    fs::remove_all(dir);
}

TEST_F(RenderHonestyValidationFixture, AssetDatabaseRejectsMalformedPersistedData)
{
    fs::path dir = MakeTempDir("rvx_asset_database_bad");
    fs::path sourceRoot = dir / "Source";
    fs::path importedRoot = dir / "Imported";
    fs::create_directories(sourceRoot);
    fs::create_directories(importedRoot);
    WriteTextFile(importedRoot / "AssetDatabase.json", "not json");

    RVX::Tools::AssetDatabase database;
    ASSERT_TRUE(database.Initialize(sourceRoot, importedRoot));
    EXPECT_FALSE(database.Load());

    fs::remove_all(dir);
}

TEST_F(RenderHonestyValidationFixture, AssetDatabaseSaveWritesDurableDatabase)
{
    fs::path dir = MakeTempDir("rvx_asset_database_save");
    fs::path sourceRoot = dir / "Source";
    fs::path importedRoot = dir / "Imported";
    fs::create_directories(sourceRoot);

    RVX::Tools::AssetDatabase database;
    ASSERT_TRUE(database.Initialize(sourceRoot, importedRoot));
    EXPECT_TRUE(database.Save());
    EXPECT_TRUE(fs::exists(importedRoot / "AssetDatabase.json"));
    EXPECT_TRUE(database.Load());

    fs::remove_all(dir);
}

TEST_F(RenderHonestyValidationFixture, AssetDatabaseReimportFailsWhenImporterReportsSuccessWithoutOutput)
{
    fs::path dir = MakeTempDir("rvx_asset_database_reimport_no_output");
    fs::path sourceRoot = dir / "Source";
    fs::path importedRoot = dir / "Imported";
    fs::create_directories(sourceRoot / "Textures");
    WriteTextFile(sourceRoot / "Textures" / "Albedo.png", "not actually a png");

    RVX::Tools::AssetDatabase database;
    ASSERT_TRUE(database.Initialize(sourceRoot, importedRoot));

    const RVX::Tools::AssetEntry* entry = database.GetAssetByPath("Textures/Albedo.png");
    ASSERT_NE(entry, nullptr);
    const RVX::Tools::AssetGUID guid = entry->guid;
    EXPECT_TRUE(entry->isDirty);

    RVX::Tools::AssetPipeline pipeline;
    pipeline.RegisterImporter(std::make_unique<SuccessWithoutOutputImporter>());

    EXPECT_FALSE(database.ReimportAsset(guid, pipeline));
    EXPECT_FALSE(fs::exists(importedRoot / "Textures" / "Albedo.rva"));

    const RVX::Tools::AssetEntry* afterImport = database.GetAsset(guid);
    ASSERT_NE(afterImport, nullptr);
    EXPECT_TRUE(afterImport->isDirty);
    EXPECT_EQ(0u, afterImport->importedModTime);

    fs::remove_all(dir);
}

TEST_F(RenderHonestyValidationFixture, AssetDatabaseReimportUnknownGuidDoesNotCreateEmptyAsset)
{
    fs::path dir = MakeTempDir("rvx_asset_database_reimport_unknown");
    fs::path sourceRoot = dir / "Source";
    fs::path importedRoot = dir / "Imported";
    fs::create_directories(sourceRoot);

    RVX::Tools::AssetDatabase database;
    ASSERT_TRUE(database.Initialize(sourceRoot, importedRoot));
    ASSERT_TRUE(database.GetAllAssets().empty());

    RVX::Tools::AssetPipeline pipeline;
    RVX::Tools::AssetGUID missingGuid;
    missingGuid.high = 1;
    missingGuid.low = 2;

    EXPECT_FALSE(database.ReimportAsset(missingGuid, pipeline));
    EXPECT_TRUE(database.GetAllAssets().empty());

    fs::remove_all(dir);
}

TEST_F(RenderHonestyValidationFixture, CookDirectoryWritesManifestForCookedOutputs)
{
    fs::path dir = MakeTempDir("rvx_cook_manifest_success");
    const fs::path sourceRoot = dir / "Source";
    const fs::path outputRoot = dir / "Cooked";
    const fs::path manifestPath = outputRoot / "CookManifest.rvxmanifest";
    fs::create_directories(outputRoot);
    const fs::path source = sourceRoot / "Textures" / "Albedo.tga";
    const std::vector<uint8_t> rgba = {
        255, 0, 0, 255, 0, 255, 0, 255,
        0, 0, 255, 255, 255, 255, 255, 255
    };
    WriteRgbaTga(source, 2, 2, rgba);
    WriteTextFile(sourceRoot / "Ignored.txt", "not importable");

    RVX::Tools::AssetPipeline pipeline;
    pipeline.RegisterImporter(std::make_unique<RVX::Tools::TextureImporter>());

    RVX::Tools::CookManifest manifest =
        pipeline.CookDirectory(sourceRoot, outputRoot, true, manifestPath);

    ASSERT_TRUE(manifest.manifestWritten) << manifest.manifestError;
    ASSERT_EQ(manifest.entries.size(), 1u);
    EXPECT_EQ(manifest.GetSuccessCount(), 1u);
    EXPECT_EQ(manifest.GetFailureCount(), 0u);

    const RVX::Tools::CookManifestEntry& entry = manifest.entries[0];
    EXPECT_EQ(entry.sourcePath, "Textures/Albedo.tga");
    EXPECT_EQ(entry.outputPath, "Textures/Albedo.rva");
    EXPECT_EQ(entry.type, RVX::Tools::AssetType::Texture);
    EXPECT_TRUE(entry.success);
    EXPECT_TRUE(entry.error.empty());
    EXPECT_GT(entry.sourceModTime, 0u);
    EXPECT_GT(entry.outputModTime, 0u);
    EXPECT_GT(entry.outputSize, 0u);
    EXPECT_EQ(entry.sourceContent.relativePath, "Textures/Albedo.tga");
    EXPECT_EQ(entry.sourceContent.byteCount, fs::file_size(source));
    EXPECT_EQ(entry.sourceContent.sha256.size(), 64u);
    EXPECT_EQ(entry.dependencies.size(), 0u);
    ASSERT_EQ(entry.artifacts.size(), 1u);
    EXPECT_EQ(entry.artifacts[0].relativePath, "Textures/Albedo.rva");
    EXPECT_EQ(entry.artifacts[0].byteCount, entry.outputSize);
    EXPECT_EQ(entry.artifacts[0].sha256.size(), 64u);
    EXPECT_EQ(entry.importerName, "TextureImporter");
    EXPECT_NE(entry.canonicalCookSettings.find("settingsSchema=RVX_COOK_SETTINGS_V1\n"),
              std::string::npos);
    EXPECT_EQ(entry.cookSettingsHash.size(), 64u);
    EXPECT_EQ(entry.recipeHash.size(), 64u);
    EXPECT_TRUE(fs::exists(outputRoot / "Textures" / "Albedo.rva"));

    const std::string manifestText = ReadBinaryFile(manifestPath);
    EXPECT_EQ(RVX::Tools::CookManifest::Version, 2u);
    EXPECT_NE(manifestText.find("RVX_COOK_MANIFEST_V2"), std::string::npos);
    EXPECT_NE(manifestText.find("schema=RVX_COOK_MANIFEST"), std::string::npos);
    EXPECT_NE(manifestText.find("version=2"), std::string::npos);
    EXPECT_NE(manifestText.find("toolName=RVXCook"), std::string::npos);
    EXPECT_NE(manifestText.find("toolVersion=2.0.0"), std::string::npos);
    EXPECT_NE(manifestText.find("entryCount=1"), std::string::npos);
    EXPECT_NE(manifestText.find("successCount=1"), std::string::npos);
    EXPECT_NE(manifestText.find("failureCount=0"), std::string::npos);
    EXPECT_NE(manifestText.find("entry.0.source=Textures/Albedo.tga"), std::string::npos);
    EXPECT_NE(manifestText.find("entry.0.output=Textures/Albedo.rva"), std::string::npos);
    EXPECT_NE(manifestText.find("entry.0.type=Texture"), std::string::npos);
    EXPECT_NE(manifestText.find("entry.0.success=1"), std::string::npos);
    EXPECT_NE(manifestText.find("entry.0.importer=TextureImporter"), std::string::npos);
    EXPECT_NE(manifestText.find("entry.0.source.byteCount="), std::string::npos);
    EXPECT_NE(manifestText.find("entry.0.source.sha256=" + entry.sourceContent.sha256),
              std::string::npos);
    EXPECT_NE(manifestText.find("entry.0.cookSettingsHash=" + entry.cookSettingsHash),
              std::string::npos);
    EXPECT_NE(manifestText.find("entry.0.recipeHash=" + entry.recipeHash),
              std::string::npos);
    EXPECT_NE(manifestText.find("entry.0.dependencyCount=0"), std::string::npos);
    EXPECT_NE(manifestText.find("entry.0.artifactCount=1"), std::string::npos);
    EXPECT_NE(manifestText.find("entry.0.artifact.0.path=Textures/Albedo.rva"),
              std::string::npos);
    EXPECT_NE(manifestText.find("entry.0.artifact.0.sha256=" + entry.artifacts[0].sha256),
              std::string::npos);
    EXPECT_NE(manifestText.find("entry.0.warningCount=0"), std::string::npos);
    EXPECT_NE(manifestText.find("RVX_COOK_MANIFEST_END"), std::string::npos);

    fs::remove_all(dir);
}

TEST_F(RenderHonestyValidationFixture, CookDirectoryManifestRejectsSuccessWithoutOutput)
{
    fs::path dir = MakeTempDir("rvx_cook_manifest_no_output");
    const fs::path sourceRoot = dir / "Source";
    const fs::path outputRoot = dir / "Cooked";
    const fs::path manifestPath = outputRoot / "CookManifest.rvxmanifest";
    fs::create_directories(outputRoot);
    WriteTextFile(sourceRoot / "Textures" / "Bad.png", "not actually a png");

    RVX::Tools::AssetPipeline pipeline;
    pipeline.RegisterImporter(std::make_unique<SuccessWithoutOutputImporter>());

    RVX::Tools::CookManifest manifest =
        pipeline.CookDirectory(sourceRoot, outputRoot, true, manifestPath);

    EXPECT_FALSE(manifest.manifestWritten);
    ASSERT_EQ(manifest.entries.size(), 1u);
    EXPECT_EQ(manifest.GetSuccessCount(), 0u);
    EXPECT_EQ(manifest.GetFailureCount(), 1u);

    const RVX::Tools::CookManifestEntry& entry = manifest.entries[0];
    EXPECT_EQ(entry.sourcePath, "Textures/Bad.png");
    EXPECT_EQ(entry.outputPath, "Textures/Bad.rva");
    EXPECT_EQ(entry.type, RVX::Tools::AssetType::Texture);
    EXPECT_FALSE(entry.success);
    EXPECT_FALSE(entry.error.empty());
    EXPECT_EQ(entry.outputModTime, 0u);
    EXPECT_EQ(entry.outputSize, 0u);
    EXPECT_TRUE(entry.sourceContent.sha256.empty());
    EXPECT_TRUE(entry.dependencies.empty());
    EXPECT_TRUE(entry.artifacts.empty());
    EXPECT_TRUE(entry.cookSettingsHash.empty());
    EXPECT_TRUE(entry.recipeHash.empty());
    EXPECT_FALSE(fs::exists(outputRoot / "Textures" / "Bad.rva"));

    EXPECT_FALSE(fs::exists(manifestPath));
    EXPECT_NE(manifest.manifestError.find("previous package was preserved"), std::string::npos);
    ExpectNoCookTransactionResidue(outputRoot);

    fs::remove_all(dir);
}

TEST_F(RenderHonestyValidationFixture,
       CookDirectoryFailureLeavesExistingPackageAndManifestByteIdentical)
{
    const fs::path dir = MakeTempDir("rvx_cook_transaction_import_failure");
    const fs::path sourceRoot = dir / "Source";
    const fs::path outputRoot = dir / "Cooked";
    const fs::path manifestPath = outputRoot / "CookManifest.rvxmanifest";
    WriteTextFile(sourceRoot / "A.ok", "first source");
    WriteTextFile(sourceRoot / "B.fail", "second source");
    WriteTextFile(outputRoot / "Existing" / "legacy.rva", "old package artifact");
    WriteTextFile(manifestPath, "old package manifest");
    const std::string oldArtifact = ReadBinaryFile(outputRoot / "Existing" / "legacy.rva");
    const std::string oldManifest = ReadBinaryFile(manifestPath);

    RVX::Tools::AssetPipeline pipeline;
    pipeline.RegisterImporter(std::make_unique<TransactionTestImporter>());
    const RVX::Tools::CookManifest manifest =
        pipeline.CookDirectory(sourceRoot, outputRoot, true, manifestPath);

    EXPECT_FALSE(manifest.manifestWritten);
    ASSERT_EQ(manifest.entries.size(), 2u);
    EXPECT_EQ(manifest.GetSuccessCount(), 1u);
    EXPECT_EQ(manifest.GetFailureCount(), 1u);
    EXPECT_EQ(ReadBinaryFile(outputRoot / "Existing" / "legacy.rva"), oldArtifact);
    EXPECT_EQ(ReadBinaryFile(manifestPath), oldManifest);
    EXPECT_FALSE(fs::exists(outputRoot / "A.rva"));
    ExpectNoCookTransactionResidue(outputRoot);

    fs::remove_all(dir);
}

TEST_F(RenderHonestyValidationFixture,
       CookDirectoryIdentityAndStagingTamperFailuresPreserveExistingPackage)
{
    const auto runFailure = [](const char* testName,
                               const std::string& sourceFile,
                               RVX::Tools::AssetPipeline::StagingMutator stagingMutator)
    {
        const fs::path dir = MakeTempDir(testName);
        const fs::path sourceRoot = dir / "Source";
        const fs::path outputRoot = dir / "Cooked";
        const fs::path manifestPath = outputRoot / "CookManifest.rvxmanifest";
        WriteTextFile(sourceRoot / sourceFile, "transaction source");
        WriteTextFile(outputRoot / "Existing" / "legacy.rva", "old package artifact");
        WriteTextFile(manifestPath, "old package manifest");
        const std::string oldArtifact = ReadBinaryFile(outputRoot / "Existing" / "legacy.rva");
        const std::string oldManifest = ReadBinaryFile(manifestPath);

        RVX::Tools::AssetPipeline pipeline;
        pipeline.RegisterImporter(std::make_unique<TransactionTestImporter>());
        const RVX::Tools::CookManifest manifest = pipeline.CookDirectory(
            sourceRoot, outputRoot, true, manifestPath, nullptr, nullptr, std::move(stagingMutator));

        EXPECT_FALSE(manifest.manifestWritten) << manifest.manifestError;
        EXPECT_EQ(ReadBinaryFile(outputRoot / "Existing" / "legacy.rva"), oldArtifact);
        EXPECT_EQ(ReadBinaryFile(manifestPath), oldManifest);
        EXPECT_FALSE(fs::exists(outputRoot / fs::path(sourceFile).replace_extension(".rva")));
        ExpectNoCookTransactionResidue(outputRoot);
        fs::remove_all(dir);
    };

    // The importer reports a product that does not exist, so identity capture
    // fails after a primary staged artifact has already been written.
    runFailure("rvx_cook_transaction_identity_failure", "Asset.missing", {});

    // This models a post-import/tamper race: Save re-hashes staged artifacts
    // immediately before publication and must fail rather than publish.
    runFailure("rvx_cook_transaction_tamper_failure",
               "Asset.ok",
               [](RVX::Tools::CookManifest&,
                  const fs::path& stagingOutputRoot,
                  std::string& outError)
               {
                   std::error_code ec;
                   fs::remove(stagingOutputRoot / "Asset.rva", ec);
                   if (ec)
                   {
                       outError = "Failed to tamper staged artifact for test: " + ec.message();
                       return false;
                   }
                   outError.clear();
                   return true;
               });
}

TEST_F(RenderHonestyValidationFixture, CookDirectorySuccessReplacesWholePackageAndExternalManifest)
{
    const fs::path dir = MakeTempDir("rvx_cook_transaction_success");
    const fs::path sourceRoot = dir / "Source";
    const fs::path outputRoot = dir / "Cooked";
    const fs::path manifestPath = dir / "PublishedManifest.rvxmanifest";
    WriteTextFile(sourceRoot / "New.ok", "new source");
    WriteTextFile(outputRoot / "Obsolete" / "old.rva", "old package artifact");
    WriteTextFile(manifestPath, "old external manifest");

    RVX::Tools::AssetPipeline pipeline;
    pipeline.RegisterImporter(std::make_unique<TransactionTestImporter>());
    const RVX::Tools::CookManifest manifest =
        pipeline.CookDirectory(sourceRoot, outputRoot, true, manifestPath);

    ASSERT_TRUE(manifest.manifestWritten) << manifest.manifestError;
    EXPECT_TRUE(fs::exists(outputRoot / "New.rva"));
    EXPECT_FALSE(fs::exists(outputRoot / "Obsolete" / "old.rva"));
    EXPECT_NE(ReadBinaryFile(manifestPath).find("successCount=1"), std::string::npos);
    EXPECT_NE(ReadBinaryFile(manifestPath).find("sourceRoot=.\n"), std::string::npos);
    EXPECT_NE(ReadBinaryFile(manifestPath).find("outputRoot=.\n"), std::string::npos);
    ExpectNoCookTransactionResidue(outputRoot);

    fs::remove_all(dir);
}

TEST_F(RenderHonestyValidationFixture, CookManifestV2IsContentHashStableAcrossEquivalentCooks)
{
    class DeterministicPngImporter final : public RVX::Tools::IAssetImporter
    {
    public:
        const char* GetName() const override { return "DeterministicPngImporter"; }
        std::vector<std::string> GetSupportedExtensions() const override { return {".png"}; }
        RVX::Tools::AssetType GetAssetType() const override { return RVX::Tools::AssetType::Texture; }

        RVX::Tools::ImportResult Import(const fs::path&,
                                        const fs::path& outputPath,
                                        const void*) override
        {
            std::ofstream output(outputPath, std::ios::binary);
            if (!output.is_open())
            {
                return {false, "Failed to write deterministic cook artifact"};
            }
            output << "cooked";
            if (!output)
            {
                return {false, "Failed while writing deterministic cook artifact"};
            }

            RVX::Tools::ImportResult result;
            result.success = true;
            result.outputPaths.push_back(outputPath.string());
            return result;
        }
    };

    fs::path dir = MakeTempDir("rvx_cook_manifest_v2_stable");
    const fs::path sourceRoot = dir / "Source";
    const fs::path outputRoot = dir / "Cooked";
    const fs::path manifestPath = outputRoot / "CookManifest.rvxmanifest";
    fs::create_directories(outputRoot);
    WriteTextFile(sourceRoot / "Textures" / "Stable.png", "abc");

    RVX::Tools::AssetPipeline pipeline;
    pipeline.RegisterImporter(std::make_unique<DeterministicPngImporter>());

    RVX::Tools::CookManifest first =
        pipeline.CookDirectory(sourceRoot, outputRoot, true, manifestPath);
    ASSERT_TRUE(first.manifestWritten) << first.manifestError;
    const std::string firstManifest = ReadBinaryFile(manifestPath);

    std::error_code timestampError;
    fs::last_write_time(sourceRoot / "Textures" / "Stable.png",
                        fs::file_time_type::clock::now(),
                        timestampError);
    ASSERT_FALSE(timestampError);

    RVX::Tools::CookManifest second =
        pipeline.CookDirectory(sourceRoot, outputRoot, true, manifestPath);
    ASSERT_TRUE(second.manifestWritten) << second.manifestError;
    EXPECT_EQ(ReadBinaryFile(manifestPath), firstManifest);
    ASSERT_EQ(first.entries.size(), 1u);
    ASSERT_EQ(second.entries.size(), 1u);
    EXPECT_EQ(first.entries[0].sourceContent.sha256,
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    EXPECT_EQ(second.entries[0].sourceContent.sha256, first.entries[0].sourceContent.sha256);
    EXPECT_EQ(second.entries[0].artifacts[0].sha256, first.entries[0].artifacts[0].sha256);
    EXPECT_EQ(second.entries[0].recipeHash, first.entries[0].recipeHash);

    const fs::path otherDir = MakeTempDir("rvx_cook_manifest_v2_stable_other_root");
    const fs::path otherSourceRoot = otherDir / "DifferentSourceRoot";
    const fs::path otherOutputRoot = otherDir / "DifferentCookedRoot";
    const fs::path otherManifestPath = otherOutputRoot / "CookManifest.rvxmanifest";
    WriteTextFile(otherSourceRoot / "Textures" / "Stable.png", "abc");

    RVX::Tools::AssetPipeline otherPipeline;
    otherPipeline.RegisterImporter(std::make_unique<DeterministicPngImporter>());
    const RVX::Tools::CookManifest third =
        otherPipeline.CookDirectory(otherSourceRoot, otherOutputRoot, true, otherManifestPath);
    ASSERT_TRUE(third.manifestWritten) << third.manifestError;
    EXPECT_EQ(ReadBinaryFile(otherManifestPath), firstManifest);

    fs::remove_all(otherDir);

    fs::remove_all(dir);
}

TEST_F(RenderHonestyValidationFixture,
       CookManifestV2CapturesExternalGltfSourceClosureAndRejectsUnsafeUris)
{
    const fs::path dir = MakeTempDir("rvx_cook_manifest_gltf_source_closure");
    const fs::path sourceRoot = dir / "Source";
    const fs::path outputRoot = dir / "Cooked";
    const fs::path manifestPath = outputRoot / "CookManifest.rvxmanifest";
    fs::create_directories(outputRoot);
    const fs::path modelPath = sourceRoot / "Models" / "Quad.gltf";
    const fs::path imagePath = sourceRoot / "Models" / "Textures" / "Albedo.tga";

    WriteMinimalQuadGltf(modelPath);
    WriteRgbaTga(imagePath, 2, 2, {
        255, 0, 0, 255, 0, 255, 0, 255,
        0, 0, 255, 255, 255, 255, 255, 255,
    });
    std::string modelJson = ReadBinaryFile(modelPath);
    const size_t buffersOffset = modelJson.find("  \"buffers\":");
    ASSERT_NE(buffersOffset, std::string::npos);
    modelJson.insert(buffersOffset,
                     "  \"images\": [ { \"uri\": \"Textures/Albedo.tga\" } ],\n");
    WriteTextFile(modelPath, modelJson);

    RVX::Tools::AssetPipeline pipeline;
    pipeline.RegisterImporter(std::make_unique<RVX::Tools::ModelImporter>());
    const RVX::Tools::CookManifest first =
        pipeline.CookDirectory(sourceRoot, outputRoot, true, manifestPath);
    ASSERT_TRUE(first.manifestWritten) << first.manifestError;
    const auto firstModel = std::find_if(first.entries.begin(), first.entries.end(),
                                         [](const RVX::Tools::CookManifestEntry& entry)
    {
        return entry.sourcePath == "Models/Quad.gltf";
    });
    ASSERT_NE(firstModel, first.entries.end());
    const RVX::Tools::CookManifestEntry& firstEntry = *firstModel;
    ASSERT_TRUE(firstEntry.sourceDependencyClosureRecorded);
    ASSERT_EQ(firstEntry.sourceDependencies.size(), 2u);
    EXPECT_EQ(firstEntry.sourceDependencies[0].relativePath, "Models/Quad.bin");
    EXPECT_EQ(firstEntry.sourceDependencies[1].relativePath, "Models/Textures/Albedo.tga");
    const std::string firstRecipe = firstEntry.recipeHash;
    const std::string firstRootHash = firstEntry.sourceContent.sha256;

    RVX::Resource::CookManifest parsed;
    std::string parseError;
    ASSERT_TRUE(RVX::Resource::LoadCookManifest(manifestPath, parsed, parseError)) << parseError;
    RVX::Resource::CookManifestExpectation expected;
    expected.selector.sourcePath = "Models/Quad.gltf";
    const RVX::Resource::CookManifestAdmissionReceipt receipt =
        RVX::Resource::VerifyCookedAssetAdmission(parsed, sourceRoot, outputRoot, expected);
    ASSERT_TRUE(receipt.IsAccepted()) << receipt.detail;
    EXPECT_EQ(receipt.observedSourceContentIdentity.scope,
              RVX::Resource::ResourceContentIdentityScope::DependencyClosure);
    EXPECT_EQ(receipt.observedSourceContentIdentity.fileCount, 3u);

    std::vector<uint8_t> alteredBin;
    const std::string originalBin = ReadBinaryFile(sourceRoot / "Models" / "Quad.bin");
    alteredBin.assign(originalBin.begin(), originalBin.end());
    ASSERT_FALSE(alteredBin.empty());
    alteredBin.front() ^= 0x01u;
    WriteBinaryFile(sourceRoot / "Models" / "Quad.bin", alteredBin);
    EXPECT_EQ(RVX::Resource::VerifyCookedAssetAdmission(parsed, sourceRoot, outputRoot, expected).code,
              RVX::Resource::CookManifestAdmissionCode::MountedContentMismatch);

    const RVX::Tools::CookManifest second =
        pipeline.CookDirectory(sourceRoot, outputRoot, true, manifestPath);
    ASSERT_TRUE(second.manifestWritten) << second.manifestError;
    const auto secondModel = std::find_if(second.entries.begin(), second.entries.end(),
                                          [](const RVX::Tools::CookManifestEntry& entry)
    {
        return entry.sourcePath == "Models/Quad.gltf";
    });
    ASSERT_NE(secondModel, second.entries.end());
    EXPECT_EQ(secondModel->sourceContent.sha256, firstRootHash);
    EXPECT_NE(secondModel->sourceDependencies[0].sha256,
              firstEntry.sourceDependencies[0].sha256);
    EXPECT_NE(secondModel->recipeHash, firstRecipe);

    const fs::path remoteRoot = dir / "RemoteSource";
    const fs::path remoteOutputRoot = dir / "RemoteCooked";
    fs::create_directories(remoteOutputRoot);
    WriteTextFile(remoteRoot / "Models" / "Remote.gltf",
                  "{\"asset\":{\"version\":\"2.0\"},\"buffers\":[{\"uri\":\"https://example.invalid/model.bin\",\"byteLength\":1}]}\n");
    const RVX::Tools::CookManifest remote = pipeline.CookDirectory(
        remoteRoot,
        remoteOutputRoot,
        true,
        remoteOutputRoot / "CookManifest.rvxmanifest");
    ASSERT_EQ(remote.entries.size(), 1u);
    EXPECT_FALSE(remote.manifestWritten);
    EXPECT_FALSE(remote.entries.front().success);

    const fs::path escapeRoot = dir / "EscapeSource";
    const fs::path escapeOutputRoot = dir / "EscapeCooked";
    fs::create_directories(escapeOutputRoot);
    WriteTextFile(escapeRoot / "Models" / "Escape.gltf",
                  "{\"asset\":{\"version\":\"2.0\"},\"buffers\":[{\"uri\":\"../outside.bin\",\"byteLength\":1}]}\n");
    const RVX::Tools::CookManifest escape = pipeline.CookDirectory(
        escapeRoot,
        escapeOutputRoot,
        true,
        escapeOutputRoot / "CookManifest.rvxmanifest");
    ASSERT_EQ(escape.entries.size(), 1u);
    EXPECT_FALSE(escape.manifestWritten);
    EXPECT_FALSE(escape.entries.front().success);

    fs::remove_all(dir);
}

TEST_F(RenderHonestyValidationFixture, CookManifestV2ChangesRecipeWhenSourceContentDrifts)
{
    fs::path dir = MakeTempDir("rvx_cook_manifest_v2_input_drift");
    const fs::path sourceRoot = dir / "Source";
    const fs::path outputRoot = dir / "Cooked";
    const fs::path manifestPath = outputRoot / "CookManifest.rvxmanifest";
    fs::create_directories(outputRoot);
    const fs::path source = sourceRoot / "Textures" / "Drift.tga";
    WriteRgbaTga(source, 2, 2, {
        1, 2, 3, 255, 4, 5, 6, 255,
        7, 8, 9, 255, 10, 11, 12, 255
    });

    RVX::Tools::AssetPipeline pipeline;
    pipeline.RegisterImporter(std::make_unique<RVX::Tools::TextureImporter>());
    RVX::Tools::CookManifest first =
        pipeline.CookDirectory(sourceRoot, outputRoot, true, manifestPath);
    ASSERT_TRUE(first.manifestWritten) << first.manifestError;

    WriteRgbaTga(source, 2, 2, {
        12, 11, 10, 255, 9, 8, 7, 255,
        6, 5, 4, 255, 3, 2, 1, 255
    });
    RVX::Tools::CookManifest second =
        pipeline.CookDirectory(sourceRoot, outputRoot, true, manifestPath);
    ASSERT_TRUE(second.manifestWritten) << second.manifestError;

    ASSERT_EQ(first.entries.size(), 1u);
    ASSERT_EQ(second.entries.size(), 1u);
    EXPECT_NE(second.entries[0].sourceContent.sha256, first.entries[0].sourceContent.sha256);
    EXPECT_NE(second.entries[0].recipeHash, first.entries[0].recipeHash);

    fs::remove_all(dir);
}

TEST_F(RenderHonestyValidationFixture, CookManifestV2ChangesRecipeWhenCookSettingsChange)
{
    fs::path dir = MakeTempDir("rvx_cook_manifest_v2_settings_drift");
    const fs::path sourceRoot = dir / "Source";
    const fs::path outputRoot = dir / "Cooked";
    const fs::path manifestPath = outputRoot / "CookManifest.rvxmanifest";
    fs::create_directories(outputRoot);
    WriteRgbaTga(sourceRoot / "Textures" / "Settings.tga", 2, 2, {
        32, 64, 128, 255, 64, 128, 32, 255,
        128, 32, 64, 255, 255, 255, 255, 255
    });

    RVX::Tools::AssetPipeline pipeline;
    pipeline.RegisterImporter(std::make_unique<RVX::Tools::TextureImporter>());
    RVX::Tools::TextureImportOptions options;
    options.compressionMode = RVX::Tools::TextureCompressionMode::BC1;
    options.compress = true;
    const auto optionsProvider = [&options](const fs::path&, RVX::Tools::AssetType) -> const void*
    {
        return &options;
    };

    RVX::Tools::CookManifest first = pipeline.CookDirectory(
        sourceRoot, outputRoot, true, manifestPath, nullptr, optionsProvider);
    ASSERT_TRUE(first.manifestWritten) << first.manifestError;

    options.compressionMode = RVX::Tools::TextureCompressionMode::BC3;
    RVX::Tools::CookManifest second = pipeline.CookDirectory(
        sourceRoot, outputRoot, true, manifestPath, nullptr, optionsProvider);
    ASSERT_TRUE(second.manifestWritten) << second.manifestError;

    ASSERT_EQ(first.entries.size(), 1u);
    ASSERT_EQ(second.entries.size(), 1u);
    EXPECT_EQ(second.entries[0].sourceContent.sha256, first.entries[0].sourceContent.sha256);
    EXPECT_NE(second.entries[0].canonicalCookSettings, first.entries[0].canonicalCookSettings);
    EXPECT_NE(second.entries[0].cookSettingsHash, first.entries[0].cookSettingsHash);
    EXPECT_NE(second.entries[0].recipeHash, first.entries[0].recipeHash);

    fs::remove_all(dir);
}

TEST_F(RenderHonestyValidationFixture, CookManifestV2RejectsTamperedOrMissingArtifacts)
{
    fs::path dir = MakeTempDir("rvx_cook_manifest_v2_artifact_validation");
    const fs::path sourceRoot = dir / "Source";
    const fs::path outputRoot = dir / "Cooked";
    const fs::path manifestPath = outputRoot / "CookManifest.rvxmanifest";
    fs::create_directories(outputRoot);
    const fs::path output = outputRoot / "Textures" / "Artifact.rva";
    WriteRgbaTga(sourceRoot / "Textures" / "Artifact.tga", 2, 2, {
        16, 32, 48, 255, 64, 80, 96, 255,
        112, 128, 144, 255, 160, 176, 192, 255
    });

    RVX::Tools::AssetPipeline pipeline;
    pipeline.RegisterImporter(std::make_unique<RVX::Tools::TextureImporter>());
    RVX::Tools::CookManifest manifest =
        pipeline.CookDirectory(sourceRoot, outputRoot, true, manifestPath);
    ASSERT_TRUE(manifest.manifestWritten) << manifest.manifestError;

    WriteTextFile(output, "tampered cooked artifact");
    std::string validationError;
    EXPECT_FALSE(manifest.Save(manifestPath, validationError));
    EXPECT_NE(validationError.find("artifact"), std::string::npos);

    manifest = pipeline.CookDirectory(sourceRoot, outputRoot, true, manifestPath);
    ASSERT_TRUE(manifest.manifestWritten) << manifest.manifestError;
    ASSERT_TRUE(fs::remove(output));
    validationError.clear();
    EXPECT_FALSE(manifest.Save(manifestPath, validationError));
    EXPECT_NE(validationError.find("artifact"), std::string::npos);

    fs::remove_all(dir);
}

TEST_F(RenderHonestyValidationFixture, RVXCookCliWritesManifestForTextureDirectory)
{
#ifndef RVX_COOK_EXECUTABLE_PATH
    GTEST_SKIP() << "RVXCook executable path was not provided by CMake";
#else
    fs::path dir = MakeTempDir("rvx_cook_cli_texture");
    const fs::path sourceRoot = dir / "Source";
    const fs::path outputRoot = dir / "Cooked";
    const fs::path manifestPath = outputRoot / "CookManifest.rvxmanifest";
    fs::create_directories(outputRoot);
    const std::vector<uint8_t> rgba = {
        255, 32, 16, 255, 32, 255, 16, 255,
        32, 16, 255, 255, 255, 255, 255, 255
    };
    WriteRgbaTga(sourceRoot / "Textures" / "Albedo.tga", 2, 2, rgba);

    const std::string command = WrapSystemCommand(
        QuoteCommandArgument(fs::path(RVX_COOK_EXECUTABLE_PATH)) +
        " --source " + QuoteCommandArgument(sourceRoot) +
        " --output " + QuoteCommandArgument(outputRoot) +
        " --manifest " + QuoteCommandArgument(manifestPath) +
        " --texture-compression bc3" +
        " --fail-on-errors");

    const int exitCode = std::system(command.c_str());
    EXPECT_EQ(exitCode, 0) << command;
    ASSERT_TRUE(fs::exists(outputRoot / "Textures" / "Albedo.rva"));
    ASSERT_TRUE(fs::exists(manifestPath));

    const std::string manifestText = ReadBinaryFile(manifestPath);
    EXPECT_NE(manifestText.find("RVX_COOK_MANIFEST_V2"), std::string::npos);
    EXPECT_NE(manifestText.find("version=2"), std::string::npos);
    EXPECT_NE(manifestText.find("entryCount=1"), std::string::npos);
    EXPECT_NE(manifestText.find("successCount=1"), std::string::npos);
    EXPECT_NE(manifestText.find("failureCount=0"), std::string::npos);
    EXPECT_NE(manifestText.find("entry.0.source=Textures/Albedo.tga"), std::string::npos);
    EXPECT_NE(manifestText.find("entry.0.output=Textures/Albedo.rva"), std::string::npos);
    EXPECT_NE(manifestText.find("entry.0.success=1"), std::string::npos);

    const std::string artifact = ReadBinaryFile(outputRoot / "Textures" / "Albedo.rva");
    EXPECT_NE(artifact.find("format=BC3"), std::string::npos);
    EXPECT_NE(artifact.find("requestedCompressionMode=BC3"), std::string::npos);
    EXPECT_NE(artifact.find("compression=BC3"), std::string::npos);

    fs::remove_all(dir);
#endif
}

TEST_F(RenderHonestyValidationFixture, RVXCookCliAppliesTextureCompressionProfileRules)
{
#ifndef RVX_COOK_EXECUTABLE_PATH
    GTEST_SKIP() << "RVXCook executable path was not provided by CMake";
#else
    fs::path dir = MakeTempDir("rvx_cook_cli_profile_texture");
    const fs::path sourceRoot = dir / "Source";
    const fs::path outputRoot = dir / "Cooked";
    const fs::path manifestPath = outputRoot / "CookManifest.rvxmanifest";
    fs::create_directories(outputRoot);
    const fs::path profilePath = dir / "CookProfile.rvxprofile";

    const std::vector<uint8_t> opaqueRgba = {
        255, 32, 16, 255, 32, 255, 16, 255,
        32, 16, 255, 255, 255, 255, 255, 255
    };
    const std::vector<uint8_t> alphaRgba = {
        255, 32, 16, 128, 32, 255, 16, 160,
        32, 16, 255, 192, 255, 255, 255, 224
    };
    const std::vector<uint8_t> heroRgba = {
        16, 64, 192, 255, 96, 128, 224, 255,
        144, 192, 80, 255, 240, 224, 128, 255
    };
    WriteRgbaTga(sourceRoot / "Textures" / "Albedo.tga", 2, 2, opaqueRgba);
    WriteRgbaTga(sourceRoot / "Textures" / "Foliage.tga", 2, 2, alphaRgba);
    WriteRgbaTga(sourceRoot / "Textures" / "Hero.tga", 2, 2, heroRgba);
    WriteTextFile(profilePath,
                  "RVX_COOK_PROFILE_V1\n"
                  "texture.compression=none\n"
                  "texture.compression[Textures/Foliage.tga]=bc3\n"
                  "texture.compression[Textures/Hero.tga]=bc7\n");

    const std::string command = WrapSystemCommand(
        QuoteCommandArgument(fs::path(RVX_COOK_EXECUTABLE_PATH)) +
        " --source " + QuoteCommandArgument(sourceRoot) +
        " --output " + QuoteCommandArgument(outputRoot) +
        " --manifest " + QuoteCommandArgument(manifestPath) +
        " --profile " + QuoteCommandArgument(profilePath) +
        " --fail-on-errors");

    const int exitCode = std::system(command.c_str());
    EXPECT_EQ(exitCode, 0) << command;
    ASSERT_TRUE(fs::exists(outputRoot / "Textures" / "Albedo.rva"));
    ASSERT_TRUE(fs::exists(outputRoot / "Textures" / "Foliage.rva"));
    ASSERT_TRUE(fs::exists(manifestPath));

    const std::string manifestText = ReadBinaryFile(manifestPath);
    EXPECT_NE(manifestText.find("entryCount=3"), std::string::npos);
    EXPECT_NE(manifestText.find("successCount=3"), std::string::npos);
    EXPECT_NE(manifestText.find("failureCount=0"), std::string::npos);

    const std::string albedoArtifact = ReadBinaryFile(outputRoot / "Textures" / "Albedo.rva");
    EXPECT_NE(albedoArtifact.find("format=RGBA8"), std::string::npos);
    EXPECT_NE(albedoArtifact.find("requestedCompression=0"), std::string::npos);
    EXPECT_NE(albedoArtifact.find("requestedCompressionMode=None"), std::string::npos);
    EXPECT_NE(albedoArtifact.find("compression=UncompressedRGBA"), std::string::npos);

    const std::string foliageArtifact = ReadBinaryFile(outputRoot / "Textures" / "Foliage.rva");
    EXPECT_NE(foliageArtifact.find("format=BC3"), std::string::npos);
    EXPECT_NE(foliageArtifact.find("requestedCompression=1"), std::string::npos);
    EXPECT_NE(foliageArtifact.find("requestedCompressionMode=BC3"), std::string::npos);
    EXPECT_NE(foliageArtifact.find("compression=BC3"), std::string::npos);

    const std::string heroArtifact = ReadBinaryFile(outputRoot / "Textures" / "Hero.rva");
    EXPECT_NE(heroArtifact.find("format=BC7"), std::string::npos);
    EXPECT_NE(heroArtifact.find("requestedCompression=1"), std::string::npos);
    EXPECT_NE(heroArtifact.find("requestedCompressionMode=BC7"), std::string::npos);
    EXPECT_NE(heroArtifact.find("compression=BC7"), std::string::npos);

    fs::remove_all(dir);
#endif
}

TEST_F(RenderHonestyValidationFixture, RVXCookCliAppliesMeshProfileLODOptions)
{
#ifndef RVX_COOK_EXECUTABLE_PATH
    GTEST_SKIP() << "RVXCook executable path was not provided by CMake";
#else
    fs::path dir = MakeTempDir("rvx_cook_cli_profile_mesh");
    const fs::path sourceRoot = dir / "Source";
    const fs::path outputRoot = dir / "Cooked";
    const fs::path manifestPath = outputRoot / "CookManifest.rvxmanifest";
    fs::create_directories(outputRoot);
    const fs::path profilePath = dir / "CookProfile.rvxprofile";
    const fs::path meshSource = sourceRoot / "Meshes" / "Quad.gltf";

    WriteMinimalQuadGltf(meshSource);
    WriteTextFile(profilePath,
                  "RVX_COOK_PROFILE_V1\n"
                  "mesh.generateTangents=true\n"
                  "mesh.optimizeMesh=false\n"
                  "mesh.generateLODs=true\n"
                  "mesh.lodCount=2\n"
                  "mesh.lodReductionFactor=0.5\n");

    const std::string command = WrapSystemCommand(
        QuoteCommandArgument(fs::path(RVX_COOK_EXECUTABLE_PATH)) +
        " --source " + QuoteCommandArgument(sourceRoot) +
        " --output " + QuoteCommandArgument(outputRoot) +
        " --manifest " + QuoteCommandArgument(manifestPath) +
        " --profile " + QuoteCommandArgument(profilePath) +
        " --fail-on-errors");

    const int exitCode = std::system(command.c_str());
    EXPECT_EQ(exitCode, 0) << command;
    ASSERT_TRUE(fs::exists(outputRoot / "Meshes" / "Quad.rva"));
    ASSERT_TRUE(fs::exists(manifestPath));

    const std::string manifestText = ReadBinaryFile(manifestPath);
    EXPECT_NE(manifestText.find("entryCount=1"), std::string::npos);
    EXPECT_NE(manifestText.find("successCount=1"), std::string::npos);
    EXPECT_NE(manifestText.find("failureCount=0"), std::string::npos);
    EXPECT_NE(manifestText.find("entry.0.source=Meshes/Quad.gltf"), std::string::npos);
    EXPECT_NE(manifestText.find("entry.0.output=Meshes/Quad.rva"), std::string::npos);
    EXPECT_NE(manifestText.find("entry.0.type=Model"), std::string::npos);
    EXPECT_NE(manifestText.find("entry.0.dependency.0.path=Meshes/Quad.rvdeps/meshes.rva"),
              std::string::npos);
    EXPECT_NE(manifestText.find("entry.0.dependency.0.sha256="), std::string::npos);

    const std::string modelArtifact = ReadBinaryFile(outputRoot / "Meshes" / "Quad.rva");
    EXPECT_NE(modelArtifact.find("RVX_MODEL_PREBAKE_V1"), std::string::npos);
    const std::string artifact =
        ReadBinaryFile(outputRoot / "Meshes" / "Quad.rvdeps" / "meshes.rva");
    EXPECT_NE(artifact.find("RVX_MESH_PREBAKE_V1"), std::string::npos);
    EXPECT_NE(artifact.find("requestedTangents=1"), std::string::npos);
    EXPECT_NE(artifact.find("requestedOptimization=0"), std::string::npos);
    EXPECT_NE(artifact.find("requestedLODs=1"), std::string::npos);
    EXPECT_NE(artifact.find("requestedLODCount=2"), std::string::npos);
    EXPECT_NE(artifact.find("writtenLODCount=2"), std::string::npos);
    EXPECT_NE(artifact.find("mesh.0.writtenLODCount=2"), std::string::npos);
    EXPECT_NE(artifact.find("mesh.0.lod.1.vertexCount=3"), std::string::npos);
    EXPECT_NE(artifact.find("mesh.0.lod.1.indexCount=3"), std::string::npos);
    EXPECT_NE(artifact.find("mesh.0.lod.1.triangleCount=1"), std::string::npos);
    EXPECT_NE(artifact.find("RVX_MESH_0_LOD_1_INDEX_DATA_BEGIN"), std::string::npos);
    EXPECT_NE(artifact.find("RVX_MESH_0_LOD_1_ATTRIBUTE_"), std::string::npos);

    fs::remove_all(dir);
#endif
}

TEST_F(RenderHonestyValidationFixture, RVXCookCliWritesBC7TextureArtifact)
{
#ifndef RVX_COOK_EXECUTABLE_PATH
    GTEST_SKIP() << "RVXCook executable path was not provided by CMake";
#else
    fs::path dir = MakeTempDir("rvx_cook_cli_bc7_texture");
    const fs::path sourceRoot = dir / "Source";
    const fs::path outputRoot = dir / "Cooked";
    const fs::path manifestPath = outputRoot / "CookManifest.rvxmanifest";
    fs::create_directories(outputRoot);

    const std::vector<uint8_t> rgba = {
        24, 48, 192, 255, 64, 96, 224, 255,
        128, 160, 96, 255, 240, 224, 128, 255
    };
    WriteRgbaTga(sourceRoot / "Textures" / "Hero.tga", 2, 2, rgba);

    const std::string command = WrapSystemCommand(
        QuoteCommandArgument(fs::path(RVX_COOK_EXECUTABLE_PATH)) +
        " --source " + QuoteCommandArgument(sourceRoot) +
        " --output " + QuoteCommandArgument(outputRoot) +
        " --manifest " + QuoteCommandArgument(manifestPath) +
        " --texture-compression bc7" +
        " --fail-on-errors");

    const int exitCode = std::system(command.c_str());
    EXPECT_EQ(exitCode, 0) << command;
    const fs::path artifactPath = outputRoot / "Textures" / "Hero.rva";
    ASSERT_TRUE(fs::exists(artifactPath));

    const std::string manifestText = ReadBinaryFile(manifestPath);
    EXPECT_NE(manifestText.find("entryCount=1"), std::string::npos);
    EXPECT_NE(manifestText.find("successCount=1"), std::string::npos);
    EXPECT_NE(manifestText.find("failureCount=0"), std::string::npos);

    const std::string artifact = ReadBinaryFile(artifactPath);
    EXPECT_NE(artifact.find("format=BC7"), std::string::npos);
    EXPECT_NE(artifact.find("usage=Color"), std::string::npos);
    EXPECT_NE(artifact.find("srgb=1"), std::string::npos);
    EXPECT_NE(artifact.find("requestedCompression=1"), std::string::npos);
    EXPECT_NE(artifact.find("requestedCompressionMode=BC7"), std::string::npos);
    EXPECT_NE(artifact.find("compression=BC7"), std::string::npos);
    EXPECT_NE(artifact.find("dataSize=32"), std::string::npos);

    std::vector<uint8_t> payload;
    ASSERT_TRUE(ExtractCookedTexturePayload(artifact, payload));
    ASSERT_EQ(payload.size(), 32u);
    EXPECT_EQ(payload[0] & 0x7Fu, 1u << 6u);

    std::array<std::array<uint8_t, 4>, 16> decodedPixels{};
    ASSERT_TRUE(DecodeBC7Mode6Block(payload.data(), decodedPixels));
    EXPECT_NEAR(decodedPixels[0][0], rgba[0], 80);
    EXPECT_NEAR(decodedPixels[0][1], rgba[1], 80);
    EXPECT_NEAR(decodedPixels[0][2], rgba[2], 80);
    EXPECT_NEAR(decodedPixels[0][3], rgba[3], 2);

    RVX::Resource::TextureLoader loader(nullptr);
    std::unique_ptr<RVX::Resource::TextureResource> texture(loader.LoadFromFile(artifactPath.string()));
    ASSERT_NE(texture, nullptr) << loader.GetLastLoadError();
    EXPECT_EQ(loader.GetLastLoadStatus(), RVX::Resource::TextureLoadStatus::Loaded);
    EXPECT_EQ(texture->GetFormat(), RVX::Resource::TextureFormat::BC7);
    EXPECT_EQ(texture->GetUsage(), RVX::Resource::TextureUsage::Color);
    EXPECT_TRUE(texture->IsSRGB());
    EXPECT_EQ(texture->GetData().size(), 32u);

    fs::remove_all(dir);
#endif
}

TEST_F(RenderHonestyValidationFixture, RVXCookCliRewritesGltfTextureUrisToCookedArtifacts)
{
#ifndef RVX_COOK_EXECUTABLE_PATH
    GTEST_SKIP() << "RVXCook executable path was not provided by CMake";
#else
    fs::path dir = MakeTempDir("rvx_cook_cli_gltf_rewrite");
    const fs::path sourceRoot = dir / "Source";
    const fs::path outputRoot = dir / "Cooked";
    const fs::path manifestPath = outputRoot / "CookManifest.rvxmanifest";
    fs::create_directories(outputRoot);
    const fs::path sourceTexture = sourceRoot / "Models" / "Textures" / "Albedo.tga";
    const fs::path sourceModel = sourceRoot / "Models" / "CookedTextureMaterial.gltf";
    const fs::path rewrittenModel = outputRoot / "Models" / "CookedTextureMaterial.gltf";

    const std::vector<uint8_t> rgba = {
        255, 32, 16, 255, 32, 255, 16, 255,
        32, 16, 255, 255, 255, 255, 255, 255
    };
    WriteRgbaTga(sourceTexture, 2, 2, rgba);
    WriteTextFile(sourceModel,
                  "{\n"
                  "  \"asset\": { \"version\": \"2.0\" },\n"
                  "  \"scene\": 0,\n"
                  "  \"scenes\": [ { \"nodes\": [0] } ],\n"
                  "  \"nodes\": [ { \"mesh\": 0 } ],\n"
                  "  \"meshes\": [\n"
                  "    {\n"
                  "      \"primitives\": [\n"
                  "        {\n"
                  "          \"attributes\": {\n"
                  "            \"POSITION\": 0,\n"
                  "            \"NORMAL\": 1,\n"
                  "            \"TEXCOORD_0\": 2\n"
                  "          },\n"
                  "          \"indices\": 3,\n"
                  "          \"material\": 0,\n"
                  "          \"mode\": 4\n"
                  "        }\n"
                  "      ]\n"
                  "    }\n"
                  "  ],\n"
                  "  \"materials\": [\n"
                  "    {\n"
                  "      \"name\": \"CookRewriteMaterial\",\n"
                  "      \"pbrMetallicRoughness\": {\n"
                  "        \"baseColorTexture\": { \"index\": 0 }\n"
                  "      }\n"
                  "    }\n"
                  "  ],\n"
                  "  \"textures\": [ { \"source\": 0 } ],\n"
                   "  \"images\": [ { \"name\": \"CookRewriteAlbedo\", \"uri\": \"Textures/Albedo.tga\" } ],\n"
                  "  \"buffers\": [\n"
                  "    {\n"
                  "      \"byteLength\": 102,\n"
                  "      \"uri\": \"data:application/octet-stream;base64,AACAvwAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAAAAAAAAAAAAIA/AAAAAAAAAAAAAIA/AAAAAAAAAAAAAIA/AAAAAAAAAAAAAIA/AAAAAAAAAD8AAIA/AAABAAIA\"\n"
                  "    }\n"
                  "  ],\n"
                  "  \"bufferViews\": [\n"
                  "    { \"buffer\": 0, \"byteOffset\": 0, \"byteLength\": 36, \"target\": 34962 },\n"
                  "    { \"buffer\": 0, \"byteOffset\": 36, \"byteLength\": 36, \"target\": 34962 },\n"
                  "    { \"buffer\": 0, \"byteOffset\": 72, \"byteLength\": 24, \"target\": 34962 },\n"
                  "    { \"buffer\": 0, \"byteOffset\": 96, \"byteLength\": 6, \"target\": 34963 }\n"
                  "  ],\n"
                  "  \"accessors\": [\n"
                  "    {\n"
                  "      \"bufferView\": 0,\n"
                  "      \"componentType\": 5126,\n"
                  "      \"count\": 3,\n"
                  "      \"type\": \"VEC3\",\n"
                  "      \"min\": [-1.0, 0.0, 0.0],\n"
                  "      \"max\": [1.0, 1.0, 0.0]\n"
                  "    },\n"
                  "    { \"bufferView\": 1, \"componentType\": 5126, \"count\": 3, \"type\": \"VEC3\" },\n"
                  "    { \"bufferView\": 2, \"componentType\": 5126, \"count\": 3, \"type\": \"VEC2\" },\n"
                  "    { \"bufferView\": 3, \"componentType\": 5123, \"count\": 3, \"type\": \"SCALAR\" }\n"
                  "  ]\n"
                  "}\n");

    const std::string command = WrapSystemCommand(
        QuoteCommandArgument(fs::path(RVX_COOK_EXECUTABLE_PATH)) +
        " --source " + QuoteCommandArgument(sourceRoot) +
        " --output " + QuoteCommandArgument(outputRoot) +
        " --manifest " + QuoteCommandArgument(manifestPath) +
        " --texture-compression bc1" +
        " --rewrite-gltf-texture-uris" +
        " --fail-on-errors");

    const int exitCode = std::system(command.c_str());
    EXPECT_EQ(exitCode, 0) << command;
    ASSERT_TRUE(fs::exists(outputRoot / "Models" / "Textures" / "Albedo.rva"));
    ASSERT_TRUE(fs::exists(outputRoot / "Models" / "CookedTextureMaterial.rva"));
    ASSERT_TRUE(fs::exists(rewrittenModel));
    ASSERT_TRUE(fs::exists(manifestPath));

    const std::string rewrittenText = ReadBinaryFile(rewrittenModel);
    EXPECT_NE(rewrittenText.find("\"uri\": \"Textures/Albedo.rva\""), std::string::npos);
    EXPECT_EQ(rewrittenText.find("Textures/Albedo.tga"), std::string::npos);

    const std::string manifestText = ReadBinaryFile(manifestPath);
    EXPECT_NE(manifestText.find("entryCount=3"), std::string::npos);
    EXPECT_NE(manifestText.find("successCount=3"), std::string::npos);
    EXPECT_NE(manifestText.find("failureCount=0"), std::string::npos);
    EXPECT_NE(manifestText.find("source=Models/CookedTextureMaterial.gltf"), std::string::npos);
    EXPECT_NE(manifestText.find("output=Models/CookedTextureMaterial.gltf"), std::string::npos);
    EXPECT_NE(manifestText.find("runtimeGltfTextureUriRewrites=1"), std::string::npos);
    EXPECT_NE(manifestText.find("artifact.0.path=Models/CookedTextureMaterial.gltf"),
              std::string::npos);

    const std::string artifact =
        ReadBinaryFile(outputRoot / "Models" / "Textures" / "Albedo.rva");
    EXPECT_NE(artifact.find("format=BC1"), std::string::npos);
    EXPECT_NE(artifact.find("compression=BC1"), std::string::npos);

    RVX::Resource::ResourceManager manager;
    RVX::Resource::ResourceManagerConfig config;
    config.asyncThreadCount = 0;
    manager.Initialize(config);

    RVX::Resource::ResourceHandle<RVX::Resource::ModelResource> model =
        manager.Load<RVX::Resource::ModelResource>(rewrittenModel.string());
    ASSERT_TRUE(model);
    ASSERT_EQ(1u, model->GetMaterialCount());

    RVX::Resource::ResourceHandle<RVX::Resource::MaterialResource> material = model->GetMaterial(0);
    ASSERT_TRUE(material);
    RVX::Resource::ResourceHandle<RVX::Resource::TextureResource> albedo = material->GetAlbedoTexture();
    ASSERT_TRUE(albedo);
    EXPECT_EQ(RVX::Resource::TextureFormat::BC1, albedo->GetFormat());
    EXPECT_EQ(RVX::Resource::TextureUsage::Color, albedo->GetUsage());
    EXPECT_TRUE(albedo->IsSRGB());
    EXPECT_FALSE(albedo->IsDefaultFallback());

    manager.Shutdown();
    fs::remove_all(dir);
#endif
}

TEST_F(RenderHonestyValidationFixture, RVXCookCliRejectsInvalidCookProfile)
{
#ifndef RVX_COOK_EXECUTABLE_PATH
    GTEST_SKIP() << "RVXCook executable path was not provided by CMake";
#else
    fs::path dir = MakeTempDir("rvx_cook_cli_invalid_profile");
    const fs::path sourceRoot = dir / "Source";
    const fs::path outputRoot = dir / "Cooked";
    const fs::path profilePath = dir / "CookProfile.rvxprofile";

    const std::vector<uint8_t> rgba = {
        255, 32, 16, 255, 32, 255, 16, 255,
        32, 16, 255, 255, 255, 255, 255, 255
    };
    WriteRgbaTga(sourceRoot / "Textures" / "Albedo.tga", 2, 2, rgba);
    WriteTextFile(profilePath,
                  "RVX_COOK_PROFILE_V1\n"
                  "texture.compression=astc\n");

    const std::string command = WrapSystemCommand(
        QuoteCommandArgument(fs::path(RVX_COOK_EXECUTABLE_PATH)) +
        " --source " + QuoteCommandArgument(sourceRoot) +
        " --output " + QuoteCommandArgument(outputRoot) +
        " --profile " + QuoteCommandArgument(profilePath));

    const int exitCode = std::system(command.c_str());
    EXPECT_NE(exitCode, 0) << command;
    EXPECT_FALSE(fs::exists(outputRoot / "Textures" / "Albedo.rva"));

    fs::remove_all(dir);
#endif
}

TEST_F(RenderHonestyValidationFixture, RVXCookCliRejectsInvalidMeshCookProfile)
{
#ifndef RVX_COOK_EXECUTABLE_PATH
    GTEST_SKIP() << "RVXCook executable path was not provided by CMake";
#else
    fs::path dir = MakeTempDir("rvx_cook_cli_invalid_mesh_profile");
    const fs::path sourceRoot = dir / "Source";
    const fs::path outputRoot = dir / "Cooked";
    const fs::path profilePath = dir / "CookProfile.rvxprofile";

    WriteMinimalQuadGltf(sourceRoot / "Meshes" / "Quad.gltf");
    WriteTextFile(profilePath,
                  "RVX_COOK_PROFILE_V1\n"
                  "mesh.generateLODs=true\n"
                  "mesh.lodCount=0\n");

    const std::string command = WrapSystemCommand(
        QuoteCommandArgument(fs::path(RVX_COOK_EXECUTABLE_PATH)) +
        " --source " + QuoteCommandArgument(sourceRoot) +
        " --output " + QuoteCommandArgument(outputRoot) +
        " --profile " + QuoteCommandArgument(profilePath));

    const int exitCode = std::system(command.c_str());
    EXPECT_NE(exitCode, 0) << command;
    EXPECT_FALSE(fs::exists(outputRoot / "Meshes" / "Quad.rva"));

    fs::remove_all(dir);
#endif
}

TEST_F(RenderHonestyValidationFixture, TextureImporterWritesCookedMipArtifact)
{
    fs::path dir = MakeTempDir("rvx_texture_importer_artifact");
    const fs::path source = dir / "Albedo.tga";
    const fs::path output = dir / "Albedo.rva";
    std::vector<uint8_t> rgba(static_cast<size_t>(4 * 4 * 4), 255u);
    for (size_t i = 0; i + 3 < rgba.size(); i += 4)
    {
        rgba[i + 0] = static_cast<uint8_t>((i / 4) * 11u);
        rgba[i + 1] = static_cast<uint8_t>(255u - ((i / 4) * 7u));
        rgba[i + 2] = 64u;
        rgba[i + 3] = 255u;
    }
    WriteRgbaTga(source, 4, 4, rgba);

    RVX::Tools::TextureImporter importer;
    RVX::Tools::TextureImportOptions options;
    options.generateMipmaps = true;
    options.compress = true;

    const RVX::Tools::ImportResult result = importer.Import(source, output, &options);
    ASSERT_TRUE(result.success) << result.error;
    ASSERT_TRUE(fs::exists(output));
    ASSERT_EQ(1u, result.outputPaths.size());
    EXPECT_EQ(output.string(), result.outputPaths[0]);
    EXPECT_TRUE(result.warnings.empty());

    const std::string artifact = ReadBinaryFile(output);
    EXPECT_NE(artifact.find("RVX_TEXTURE_PREBAKE_V1"), std::string::npos);
    EXPECT_NE(artifact.find("width=4"), std::string::npos);
    EXPECT_NE(artifact.find("height=4"), std::string::npos);
    EXPECT_NE(artifact.find("mipLevels=3"), std::string::npos);
    EXPECT_NE(artifact.find("format=BC1"), std::string::npos);
    EXPECT_NE(artifact.find("usage=Color"), std::string::npos);
    EXPECT_NE(artifact.find("requestedCompression=1"), std::string::npos);
    EXPECT_NE(artifact.find("compression=BC1"), std::string::npos);
    EXPECT_NE(artifact.find("dataSize=24"), std::string::npos);
    EXPECT_NE(artifact.find("RVX_TEXTURE_DATA_BEGIN"), std::string::npos);
    EXPECT_NE(artifact.find("RVX_TEXTURE_PREBAKE_END"), std::string::npos);

    fs::remove_all(dir);
}

TEST_F(RenderHonestyValidationFixture, TextureLoaderReadsCookedTextureArtifact)
{
    fs::path dir = MakeTempDir("rvx_texture_loader_cooked_artifact");
    const fs::path source = dir / "Albedo.tga";
    const fs::path output = dir / "Albedo.rva";
    std::vector<uint8_t> rgba(static_cast<size_t>(4 * 4 * 4), 255u);
    for (size_t i = 0; i + 3 < rgba.size(); i += 4)
    {
        rgba[i + 0] = static_cast<uint8_t>((i / 4) * 13u);
        rgba[i + 1] = static_cast<uint8_t>(255u - ((i / 4) * 5u));
        rgba[i + 2] = 32u;
        rgba[i + 3] = 255u;
    }
    WriteRgbaTga(source, 4, 4, rgba);

    RVX::Tools::TextureImporter importer;
    RVX::Tools::TextureImportOptions importOptions;
    importOptions.generateMipmaps = true;
    importOptions.compress = false;

    const RVX::Tools::ImportResult importResult = importer.Import(source, output, &importOptions);
    ASSERT_TRUE(importResult.success) << importResult.error;

    RVX::Resource::TextureLoader loader(nullptr);
    EXPECT_TRUE(loader.CanLoad(output.string()));
    std::unique_ptr<RVX::Resource::TextureResource> texture(loader.LoadFromFile(output.string()));
    ASSERT_NE(texture, nullptr) << loader.GetLastLoadError();
    EXPECT_EQ(loader.GetLastLoadStatus(), RVX::Resource::TextureLoadStatus::Loaded);
    EXPECT_FALSE(texture->IsDefaultFallback());

    const RVX::Resource::TextureMetadata& metadata = texture->GetMetadata();
    EXPECT_EQ(metadata.width, 4u);
    EXPECT_EQ(metadata.height, 4u);
    EXPECT_EQ(metadata.depth, 1u);
    EXPECT_EQ(metadata.mipLevels, 3u);
    EXPECT_EQ(metadata.arrayLayers, 1u);
    EXPECT_EQ(metadata.format, RVX::Resource::TextureFormat::RGBA8);
    EXPECT_EQ(metadata.usage, RVX::Resource::TextureUsage::Color);
    EXPECT_TRUE(metadata.isSRGB);
    ASSERT_EQ(texture->GetData().size(), 84u);
    EXPECT_EQ(texture->GetData()[0], 0u);
    EXPECT_EQ(texture->GetData()[1], 255u);
    EXPECT_EQ(texture->GetData()[2], 32u);
    EXPECT_EQ(texture->GetData()[3], 255u);

    fs::remove_all(dir);
}

TEST_F(RenderHonestyValidationFixture, TextureLoaderReadsCookedBC1TextureArtifact)
{
    fs::path dir = MakeTempDir("rvx_texture_loader_bc1_artifact");
    const fs::path source = dir / "Albedo.tga";
    const fs::path output = dir / "Albedo.rva";
    std::vector<uint8_t> rgba(static_cast<size_t>(4 * 4 * 4), 255u);
    for (size_t i = 0; i + 3 < rgba.size(); i += 4)
    {
        rgba[i + 0] = static_cast<uint8_t>((i / 4) * 9u);
        rgba[i + 1] = static_cast<uint8_t>(255u - ((i / 4) * 3u));
        rgba[i + 2] = 48u;
        rgba[i + 3] = 255u;
    }
    WriteRgbaTga(source, 4, 4, rgba);

    RVX::Tools::TextureImporter importer;
    RVX::Tools::TextureImportOptions importOptions;
    importOptions.generateMipmaps = true;
    importOptions.compress = true;

    const RVX::Tools::ImportResult importResult = importer.Import(source, output, &importOptions);
    ASSERT_TRUE(importResult.success) << importResult.error;
    EXPECT_TRUE(importResult.warnings.empty());

    RVX::Resource::TextureLoader loader(nullptr);
    std::unique_ptr<RVX::Resource::TextureResource> texture(loader.LoadFromFile(output.string()));
    ASSERT_NE(texture, nullptr) << loader.GetLastLoadError();
    EXPECT_EQ(loader.GetLastLoadStatus(), RVX::Resource::TextureLoadStatus::Loaded);

    const RVX::Resource::TextureMetadata& metadata = texture->GetMetadata();
    EXPECT_EQ(metadata.width, 4u);
    EXPECT_EQ(metadata.height, 4u);
    EXPECT_EQ(metadata.depth, 1u);
    EXPECT_EQ(metadata.mipLevels, 3u);
    EXPECT_EQ(metadata.arrayLayers, 1u);
    EXPECT_EQ(metadata.format, RVX::Resource::TextureFormat::BC1);
    EXPECT_EQ(metadata.usage, RVX::Resource::TextureUsage::Color);
    EXPECT_TRUE(metadata.isSRGB);
    EXPECT_EQ(texture->GetData().size(), 24u);

    fs::remove_all(dir);
}

TEST_F(RenderHonestyValidationFixture, ModelLoaderResolvesExternalCookedTextureArtifact)
{
    fs::path dir = MakeTempDir("rvx_model_loader_cooked_texture_uri");
    const fs::path source = dir / "Albedo.tga";
    const fs::path cooked = dir / "Albedo.rva";
    const fs::path modelPath = dir / "CookedTextureMaterial.gltf";

    std::vector<uint8_t> rgba(static_cast<size_t>(4 * 4 * 4), 255u);
    for (size_t i = 0; i + 3 < rgba.size(); i += 4)
    {
        rgba[i + 0] = static_cast<uint8_t>(64u + ((i / 4) * 5u));
        rgba[i + 1] = static_cast<uint8_t>(128u + ((i / 4) * 3u));
        rgba[i + 2] = static_cast<uint8_t>(32u + ((i / 4) * 7u));
        rgba[i + 3] = 255u;
    }
    WriteRgbaTga(source, 4, 4, rgba);

    RVX::Tools::TextureImporter importer;
    RVX::Tools::TextureImportOptions importOptions;
    importOptions.generateMipmaps = true;
    importOptions.compress = true;

    const RVX::Tools::ImportResult importResult = importer.Import(source, cooked, &importOptions);
    ASSERT_TRUE(importResult.success) << importResult.error;

    WriteTextFile(modelPath,
                  "{\n"
                  "  \"asset\": { \"version\": \"2.0\" },\n"
                  "  \"scene\": 0,\n"
                  "  \"scenes\": [ { \"nodes\": [0] } ],\n"
                  "  \"nodes\": [ { \"mesh\": 0 } ],\n"
                  "  \"meshes\": [\n"
                  "    {\n"
                  "      \"primitives\": [\n"
                  "        {\n"
                  "          \"attributes\": {\n"
                  "            \"POSITION\": 0,\n"
                  "            \"NORMAL\": 1,\n"
                  "            \"TEXCOORD_0\": 2\n"
                  "          },\n"
                  "          \"indices\": 3,\n"
                  "          \"material\": 0,\n"
                  "          \"mode\": 4\n"
                  "        }\n"
                  "      ]\n"
                  "    }\n"
                  "  ],\n"
                  "  \"materials\": [\n"
                  "    {\n"
                  "      \"name\": \"CookedBCMaterial\",\n"
                  "      \"pbrMetallicRoughness\": {\n"
                  "        \"baseColorTexture\": { \"index\": 0 }\n"
                  "      }\n"
                  "    }\n"
                  "  ],\n"
                  "  \"textures\": [ { \"source\": 0 } ],\n"
                  "  \"images\": [ { \"name\": \"CookedAlbedo\", \"uri\": \"Albedo.rva\" } ],\n"
                  "  \"buffers\": [\n"
                  "    {\n"
                  "      \"byteLength\": 102,\n"
                  "      \"uri\": \"data:application/octet-stream;base64,AACAvwAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAAAAAAAAAAAAIA/AAAAAAAAAAAAAIA/AAAAAAAAAAAAAIA/AAAAAAAAAAAAAIA/AAAAAAAAAD8AAIA/AAABAAIA\"\n"
                  "    }\n"
                  "  ],\n"
                  "  \"bufferViews\": [\n"
                  "    { \"buffer\": 0, \"byteOffset\": 0, \"byteLength\": 36, \"target\": 34962 },\n"
                  "    { \"buffer\": 0, \"byteOffset\": 36, \"byteLength\": 36, \"target\": 34962 },\n"
                  "    { \"buffer\": 0, \"byteOffset\": 72, \"byteLength\": 24, \"target\": 34962 },\n"
                  "    { \"buffer\": 0, \"byteOffset\": 96, \"byteLength\": 6, \"target\": 34963 }\n"
                  "  ],\n"
                  "  \"accessors\": [\n"
                  "    {\n"
                  "      \"bufferView\": 0,\n"
                  "      \"componentType\": 5126,\n"
                  "      \"count\": 3,\n"
                  "      \"type\": \"VEC3\",\n"
                  "      \"min\": [-1.0, 0.0, 0.0],\n"
                  "      \"max\": [1.0, 1.0, 0.0]\n"
                  "    },\n"
                  "    { \"bufferView\": 1, \"componentType\": 5126, \"count\": 3, \"type\": \"VEC3\" },\n"
                  "    { \"bufferView\": 2, \"componentType\": 5126, \"count\": 3, \"type\": \"VEC2\" },\n"
                  "    { \"bufferView\": 3, \"componentType\": 5123, \"count\": 3, \"type\": \"SCALAR\" }\n"
                  "  ]\n"
                  "}\n");

    RVX::Resource::GLTFImporter gltfImporter;
    RVX::Resource::GLTFImportResult gltfResult = gltfImporter.Import(modelPath.string());
    ASSERT_TRUE(gltfResult.success) << gltfResult.errorMessage;
    ASSERT_EQ(1u, gltfResult.textures.size());
    EXPECT_TRUE(gltfResult.warnings.empty());
    EXPECT_EQ(RVX::Resource::TextureSourceType::External, gltfResult.textures[0].sourceType);
    EXPECT_EQ("Albedo.rva", gltfResult.textures[0].path);
    EXPECT_EQ(RVX::Resource::TextureUsage::Color, gltfResult.textures[0].usage);
    EXPECT_TRUE(gltfResult.textures[0].isSRGB);

    RVX::Resource::ResourceManager manager;
    RVX::Resource::ResourceManagerConfig config;
    config.asyncThreadCount = 0;
    manager.Initialize(config);

    RVX::Resource::ResourceHandle<RVX::Resource::ModelResource> model =
        manager.Load<RVX::Resource::ModelResource>(modelPath.string());
    ASSERT_TRUE(model);
    ASSERT_EQ(1u, model->GetMaterialCount());

    RVX::Resource::ResourceHandle<RVX::Resource::MaterialResource> material = model->GetMaterial(0);
    ASSERT_TRUE(material);
    RVX::Resource::ResourceHandle<RVX::Resource::TextureResource> albedo = material->GetAlbedoTexture();
    ASSERT_TRUE(albedo);
    EXPECT_EQ(RVX::Resource::TextureFormat::BC1, albedo->GetFormat());
    EXPECT_EQ(RVX::Resource::TextureUsage::Color, albedo->GetUsage());
    EXPECT_TRUE(albedo->IsSRGB());
    EXPECT_EQ(24u, albedo->GetData().size());
    EXPECT_FALSE(albedo->IsDefaultFallback());

    manager.Shutdown();

    // Prepared model loading must recognize an RVA texture as an already
    // cooked CPU product. It is published directly and never enters the
    // source-image fallback/decode streaming state machine.
    RVX::Resource::ResourceManager preparedManager;
    preparedManager.Initialize(config);
    auto preparedRequest =
        preparedManager.RequestAsync<RVX::Resource::ModelResource>(
            modelPath.string());
    ASSERT_TRUE(preparedRequest);
    preparedManager.ProcessCompletedLoads();
    RVX::Resource::ResourceHandle<RVX::Resource::ModelResource>
        preparedModel = preparedRequest.TryGet();
    ASSERT_TRUE(preparedModel);
    EXPECT_EQ(preparedModel->GetTextureStreamingSnapshot().stage,
              RVX::Resource::ModelTextureStreamingStage::None);
    ASSERT_EQ(preparedModel->GetMaterialCount(), 1u);
    auto preparedAlbedo =
        preparedModel->GetMaterial(0)->GetAlbedoTexture();
    ASSERT_TRUE(preparedAlbedo);
    EXPECT_EQ(preparedAlbedo->GetFormat(),
              RVX::Resource::TextureFormat::BC1);
    EXPECT_EQ(preparedAlbedo->GetData().size(), 24u);
    EXPECT_FALSE(preparedAlbedo->IsStreamingPlaceholder());
    EXPECT_EQ(preparedManager.GetModelTextureStreamingStats().completedDecodes,
              0u);
    preparedManager.Shutdown();

    fs::remove_all(dir);
}

TEST_F(RenderHonestyValidationFixture, TextureImporterWritesCookedBC3AlphaArtifact)
{
    fs::path dir = MakeTempDir("rvx_texture_importer_bc3_alpha");
    const fs::path source = dir / "Foliage_albedo.tga";
    const fs::path output = dir / "Foliage_albedo.rva";
    std::vector<uint8_t> rgba(static_cast<size_t>(4 * 4 * 4), 255u);
    for (size_t i = 0; i + 3 < rgba.size(); i += 4)
    {
        const uint8_t pixel = static_cast<uint8_t>(i / 4);
        rgba[i + 0] = static_cast<uint8_t>(32u + pixel * 7u);
        rgba[i + 1] = static_cast<uint8_t>(96u + pixel * 5u);
        rgba[i + 2] = static_cast<uint8_t>(48u + pixel * 3u);
        rgba[i + 3] = static_cast<uint8_t>(64u + pixel * 11u);
    }
    WriteRgbaTga(source, 4, 4, rgba);

    RVX::Tools::TextureImporter importer;
    RVX::Tools::TextureImportOptions importOptions;
    importOptions.generateMipmaps = true;
    importOptions.compress = true;

    const RVX::Tools::ImportResult importResult = importer.Import(source, output, &importOptions);
    ASSERT_TRUE(importResult.success) << importResult.error;
    EXPECT_TRUE(importResult.warnings.empty());

    const std::string artifact = ReadBinaryFile(output);
    EXPECT_NE(artifact.find("format=BC3"), std::string::npos);
    EXPECT_NE(artifact.find("usage=Color"), std::string::npos);
    EXPECT_NE(artifact.find("srgb=1"), std::string::npos);
    EXPECT_NE(artifact.find("requestedCompression=1"), std::string::npos);
    EXPECT_NE(artifact.find("compression=BC3"), std::string::npos);
    EXPECT_NE(artifact.find("dataSize=48"), std::string::npos);

    RVX::Resource::TextureLoader loader(nullptr);
    std::unique_ptr<RVX::Resource::TextureResource> texture(loader.LoadFromFile(output.string()));
    ASSERT_NE(texture, nullptr) << loader.GetLastLoadError();
    EXPECT_EQ(loader.GetLastLoadStatus(), RVX::Resource::TextureLoadStatus::Loaded);

    const RVX::Resource::TextureMetadata& metadata = texture->GetMetadata();
    EXPECT_EQ(metadata.width, 4u);
    EXPECT_EQ(metadata.height, 4u);
    EXPECT_EQ(metadata.mipLevels, 3u);
    EXPECT_EQ(metadata.format, RVX::Resource::TextureFormat::BC3);
    EXPECT_EQ(metadata.usage, RVX::Resource::TextureUsage::Color);
    EXPECT_TRUE(metadata.isSRGB);
    EXPECT_EQ(texture->GetData().size(), 48u);

    fs::remove_all(dir);
}

TEST_F(RenderHonestyValidationFixture, TextureImporterHonorsExplicitBC3CompressionMode)
{
    fs::path dir = MakeTempDir("rvx_texture_importer_explicit_bc3");
    const fs::path source = dir / "Albedo.tga";
    const fs::path output = dir / "Albedo.rva";
    std::vector<uint8_t> rgba(static_cast<size_t>(4 * 4 * 4), 255u);
    for (size_t i = 0; i + 3 < rgba.size(); i += 4)
    {
        const uint8_t pixel = static_cast<uint8_t>(i / 4);
        rgba[i + 0] = static_cast<uint8_t>(16u + pixel * 7u);
        rgba[i + 1] = static_cast<uint8_t>(192u - pixel * 5u);
        rgba[i + 2] = static_cast<uint8_t>(64u + pixel * 3u);
        rgba[i + 3] = 255u;
    }
    WriteRgbaTga(source, 4, 4, rgba);

    RVX::Tools::TextureImporter importer;
    RVX::Tools::TextureImportOptions importOptions;
    importOptions.generateMipmaps = true;
    importOptions.compress = true;
    importOptions.compressionMode = RVX::Tools::TextureCompressionMode::BC3;

    const RVX::Tools::ImportResult importResult = importer.Import(source, output, &importOptions);
    ASSERT_TRUE(importResult.success) << importResult.error;
    EXPECT_TRUE(importResult.warnings.empty());

    const std::string artifact = ReadBinaryFile(output);
    EXPECT_NE(artifact.find("format=BC3"), std::string::npos);
    EXPECT_NE(artifact.find("requestedCompression=1"), std::string::npos);
    EXPECT_NE(artifact.find("requestedCompressionMode=BC3"), std::string::npos);
    EXPECT_NE(artifact.find("compression=BC3"), std::string::npos);
    EXPECT_NE(artifact.find("dataSize=48"), std::string::npos);

    fs::remove_all(dir);
}

TEST_F(RenderHonestyValidationFixture, TextureImporterRejectsExplicitBC1ForAlphaTexture)
{
    fs::path dir = MakeTempDir("rvx_texture_importer_explicit_bc1_alpha");
    const fs::path source = dir / "Foliage_albedo.tga";
    const fs::path output = dir / "Foliage_albedo.rva";
    std::vector<uint8_t> rgba(static_cast<size_t>(4 * 4 * 4), 255u);
    for (size_t i = 0; i + 3 < rgba.size(); i += 4)
    {
        const uint8_t pixel = static_cast<uint8_t>(i / 4);
        rgba[i + 0] = static_cast<uint8_t>(32u + pixel * 7u);
        rgba[i + 1] = static_cast<uint8_t>(96u + pixel * 5u);
        rgba[i + 2] = static_cast<uint8_t>(48u + pixel * 3u);
        rgba[i + 3] = static_cast<uint8_t>(64u + pixel * 11u);
    }
    WriteRgbaTga(source, 4, 4, rgba);

    RVX::Tools::TextureImporter importer;
    RVX::Tools::TextureImportOptions importOptions;
    importOptions.generateMipmaps = true;
    importOptions.compress = true;
    importOptions.compressionMode = RVX::Tools::TextureCompressionMode::BC1;

    const RVX::Tools::ImportResult importResult = importer.Import(source, output, &importOptions);
    EXPECT_FALSE(importResult.success);
    EXPECT_NE(importResult.error.find("BC1"), std::string::npos);
    EXPECT_FALSE(fs::exists(output));

    fs::remove_all(dir);
}

TEST_F(RenderHonestyValidationFixture, TextureImporterWritesCookedBC5NormalArtifact)
{
    fs::path dir = MakeTempDir("rvx_texture_importer_bc5_normal");
    const fs::path source = dir / "Wall_normal.tga";
    const fs::path output = dir / "Wall_normal.rva";
    std::vector<uint8_t> rgba(static_cast<size_t>(4 * 4 * 4), 255u);
    for (size_t i = 0; i + 3 < rgba.size(); i += 4)
    {
        const uint8_t pixel = static_cast<uint8_t>(i / 4);
        rgba[i + 0] = static_cast<uint8_t>(96u + pixel * 3u);
        rgba[i + 1] = static_cast<uint8_t>(128u + pixel * 2u);
        rgba[i + 2] = 255u;
        rgba[i + 3] = 255u;
    }
    WriteRgbaTga(source, 4, 4, rgba);

    RVX::Tools::TextureImporter importer;
    RVX::Tools::TextureImportOptions importOptions;
    importOptions.generateMipmaps = true;
    importOptions.compress = true;

    const RVX::Tools::ImportResult importResult = importer.Import(source, output, &importOptions);
    ASSERT_TRUE(importResult.success) << importResult.error;
    EXPECT_TRUE(importResult.warnings.empty());

    const std::string artifact = ReadBinaryFile(output);
    EXPECT_NE(artifact.find("format=BC5"), std::string::npos);
    EXPECT_NE(artifact.find("usage=Normal"), std::string::npos);
    EXPECT_NE(artifact.find("srgb=0"), std::string::npos);
    EXPECT_NE(artifact.find("requestedCompression=1"), std::string::npos);
    EXPECT_NE(artifact.find("compression=BC5"), std::string::npos);
    EXPECT_NE(artifact.find("dataSize=48"), std::string::npos);

    RVX::Resource::TextureLoader loader(nullptr);
    std::unique_ptr<RVX::Resource::TextureResource> texture(loader.LoadFromFile(output.string()));
    ASSERT_NE(texture, nullptr) << loader.GetLastLoadError();
    EXPECT_EQ(loader.GetLastLoadStatus(), RVX::Resource::TextureLoadStatus::Loaded);

    const RVX::Resource::TextureMetadata& metadata = texture->GetMetadata();
    EXPECT_EQ(metadata.width, 4u);
    EXPECT_EQ(metadata.height, 4u);
    EXPECT_EQ(metadata.mipLevels, 3u);
    EXPECT_EQ(metadata.format, RVX::Resource::TextureFormat::BC5);
    EXPECT_EQ(metadata.usage, RVX::Resource::TextureUsage::Normal);
    EXPECT_FALSE(metadata.isSRGB);
    EXPECT_EQ(texture->GetData().size(), 48u);

    fs::remove_all(dir);
}

TEST_F(RenderHonestyValidationFixture, ResourceManagerDispatchesCookedTextureArtifactByHeader)
{
    fs::path dir = MakeTempDir("rvx_resource_manager_cooked_texture");
    const fs::path source = dir / "Albedo.tga";
    const fs::path output = dir / "Albedo.rva";
    std::vector<uint8_t> rgba(static_cast<size_t>(2 * 2 * 4), 255u);
    rgba[0] = 7u;
    rgba[1] = 17u;
    rgba[2] = 27u;
    rgba[3] = 255u;
    WriteRgbaTga(source, 2, 2, rgba);

    RVX::Tools::TextureImporter importer;
    RVX::Tools::TextureImportOptions importOptions;
    importOptions.generateMipmaps = true;
    importOptions.compress = false;
    ASSERT_TRUE(importer.Import(source, output, &importOptions).success);

    RVX::Resource::ResourceManager manager;
    manager.Initialize();

    RVX::IResource* resource = manager.LoadResource(output.string());
    ASSERT_NE(resource, nullptr);
    EXPECT_EQ(resource->GetType(), RVX::ResourceType::Texture);
    auto* texture = dynamic_cast<RVX::Resource::TextureResource*>(resource);
    ASSERT_NE(texture, nullptr);
    EXPECT_EQ(texture->GetMetadata().width, 2u);
    EXPECT_EQ(texture->GetMetadata().height, 2u);
    EXPECT_EQ(texture->GetMetadata().mipLevels, 2u);
    ASSERT_EQ(texture->GetData().size(), 20u);
    EXPECT_EQ(texture->GetData()[0], 7u);
    EXPECT_EQ(texture->GetData()[1], 17u);
    EXPECT_EQ(texture->GetData()[2], 27u);

    manager.Shutdown();
    fs::remove_all(dir);
}

TEST_F(RenderHonestyValidationFixture, ResourceManagerRejectsUnknownCookedArtifactHeader)
{
    fs::path dir = MakeTempDir("rvx_resource_manager_unknown_artifact");
    const fs::path artifact = dir / "Unknown.rva";
    WriteTextFile(artifact, "RVX_UNKNOWN_PREBAKE_V1\n");

    RVX::Resource::ResourceManager manager;
    manager.Initialize();

    EXPECT_EQ(manager.LoadResource(artifact.string()), nullptr);

    manager.Shutdown();
    fs::remove_all(dir);
}

TEST_F(RenderHonestyValidationFixture, AssetDatabaseReimportAcceptsTextureImporterWhenArtifactIsWritten)
{
    fs::path dir = MakeTempDir("rvx_asset_database_texture_reimport");
    fs::path sourceRoot = dir / "Source";
    fs::path importedRoot = dir / "Imported";
    fs::create_directories(sourceRoot / "Textures");
    const fs::path source = sourceRoot / "Textures" / "Albedo.tga";
    std::vector<uint8_t> rgba(static_cast<size_t>(2 * 2 * 4), 255u);
    rgba[0] = 255u;
    rgba[1] = 0u;
    rgba[2] = 0u;
    rgba[4] = 0u;
    rgba[5] = 255u;
    rgba[6] = 0u;
    rgba[8] = 0u;
    rgba[9] = 0u;
    rgba[10] = 255u;
    WriteRgbaTga(source, 2, 2, rgba);

    RVX::Tools::AssetDatabase database;
    ASSERT_TRUE(database.Initialize(sourceRoot, importedRoot));
    const RVX::Tools::AssetEntry* entry = database.GetAssetByPath("Textures/Albedo.tga");
    ASSERT_NE(entry, nullptr);
    const RVX::Tools::AssetGUID guid = entry->guid;
    EXPECT_EQ(RVX::Tools::AssetType::Texture, entry->type);
    EXPECT_TRUE(entry->isDirty);

    RVX::Tools::AssetPipeline pipeline;
    pipeline.RegisterImporter(std::make_unique<RVX::Tools::TextureImporter>());

    ASSERT_TRUE(database.ReimportAsset(guid, pipeline));
    const fs::path importedPath = importedRoot / "Textures" / "Albedo.rva";
    ASSERT_TRUE(fs::exists(importedPath));
    const std::string artifact = ReadBinaryFile(importedPath);
    EXPECT_NE(artifact.find("RVX_TEXTURE_PREBAKE_V1"), std::string::npos);
    EXPECT_NE(artifact.find("mipLevels=2"), std::string::npos);

    const RVX::Tools::AssetEntry* afterImport = database.GetAsset(guid);
    ASSERT_NE(afterImport, nullptr);
    EXPECT_FALSE(afterImport->isDirty);
    EXPECT_NE(0u, afterImport->importedModTime);

    fs::remove_all(dir);
}

TEST_F(RenderHonestyValidationFixture, MeshGenerateTangentsUsesUInt16Indices)
{
    RVX::Mesh mesh;
    mesh.name = "UInt16IndexedQuad";
    mesh.SetPositions({
        {0.0f, 0.0f, 0.0f},
        {1.0f, 0.0f, 0.0f},
        {1.0f, 1.0f, 0.0f},
        {0.0f, 1.0f, 0.0f},
    });
    mesh.SetNormals({
        {0.0f, 0.0f, 1.0f},
        {0.0f, 0.0f, 1.0f},
        {0.0f, 0.0f, 1.0f},
        {0.0f, 0.0f, 1.0f},
    });
    mesh.SetUVs({
        {0.0f, 0.0f},
        {0.0f, 1.0f},
        {1.0f, 1.0f},
        {1.0f, 0.0f},
    });
    mesh.SetIndices(std::vector<uint16_t>{0, 1, 2, 0, 2, 3});

    ASSERT_TRUE(mesh.GenerateTangents());
    const RVX::VertexAttribute* tangents = mesh.GetAttribute(RVX::VertexBufferNames::Tangent);
    ASSERT_NE(tangents, nullptr);
    ASSERT_EQ(4u, tangents->GetVertexCount());

    const float* tangentData = static_cast<const float*>(tangents->GetData());
    EXPECT_NEAR(tangentData[0], 0.0f, 1.0e-4f);
    EXPECT_GT(tangentData[1], 0.9f);
    EXPECT_NEAR(tangentData[2], 0.0f, 1.0e-4f);
    EXPECT_NEAR(std::abs(tangentData[3]), 1.0f, 1.0e-4f);
}

TEST_F(RenderHonestyValidationFixture, MeshImporterWritesCookedTangentArtifact)
{
    fs::path dir = MakeTempDir("rvx_mesh_importer_artifact");
    const fs::path source = dir / "Quad.gltf";
    const fs::path output = dir / "Quad.rva";
    WriteMinimalQuadGltf(source);

    RVX::Tools::MeshImporter importer;
    RVX::Tools::MeshImportOptions options;
    options.generateTangents = true;
    options.generateLODs = true;
    options.lodCount = 2;

    const RVX::Tools::ImportResult result = importer.Import(source, output, &options);
    ASSERT_TRUE(result.success) << result.error;
    ASSERT_TRUE(fs::exists(output));
    ASSERT_EQ(1u, result.outputPaths.size());
    EXPECT_EQ(output.string(), result.outputPaths[0]);
    ASSERT_FALSE(result.warnings.empty());

    const std::string artifact = ReadBinaryFile(output);
    EXPECT_NE(artifact.find("RVX_MESH_PREBAKE_V1"), std::string::npos);
    EXPECT_NE(artifact.find("meshCount=1"), std::string::npos);
    EXPECT_NE(artifact.find("requestedTangents=1"), std::string::npos);
    EXPECT_NE(artifact.find("requestedLODs=1"), std::string::npos);
    EXPECT_NE(artifact.find("writtenLODCount=2"), std::string::npos);
    EXPECT_NE(artifact.find("mesh.0.writtenLODCount=2"), std::string::npos);
    EXPECT_NE(artifact.find("mesh.0.name=CookQuad"), std::string::npos);
    EXPECT_NE(artifact.find("mesh.0.vertexCount=4"), std::string::npos);
    EXPECT_NE(artifact.find("mesh.0.indexCount=6"), std::string::npos);
    EXPECT_NE(artifact.find("mesh.0.hasTangents=1"), std::string::npos);
    EXPECT_NE(artifact.find("mesh.0.lod.1.vertexCount=3"), std::string::npos);
    EXPECT_NE(artifact.find("mesh.0.lod.1.indexCount=3"), std::string::npos);
    EXPECT_NE(artifact.find("mesh.0.lod.1.triangleCount=1"), std::string::npos);
    EXPECT_NE(artifact.find(".name=tangent"), std::string::npos);
    EXPECT_NE(artifact.find("RVX_MESH_0_INDEX_DATA_BEGIN"), std::string::npos);
    EXPECT_NE(artifact.find("RVX_MESH_0_LOD_1_INDEX_DATA_BEGIN"), std::string::npos);
    EXPECT_NE(artifact.find("RVX_MESH_0_LOD_1_ATTRIBUTE_"), std::string::npos);
    EXPECT_NE(artifact.find("RVX_MESH_PREBAKE_END"), std::string::npos);

    fs::remove_all(dir);
}

TEST_F(RenderHonestyValidationFixture, AssetDatabaseReimportAcceptsModelImporterWhenArtifactIsWritten)
{
    fs::path dir = MakeTempDir("rvx_asset_database_mesh_reimport");
    fs::path sourceRoot = dir / "Source";
    fs::path importedRoot = dir / "Imported";
    fs::create_directories(sourceRoot / "Meshes");
    const fs::path source = sourceRoot / "Meshes" / "Quad.gltf";
    WriteMinimalQuadGltf(source);

    RVX::Tools::AssetDatabase database;
    ASSERT_TRUE(database.Initialize(sourceRoot, importedRoot));
    const RVX::Tools::AssetEntry* entry = database.GetAssetByPath("Meshes/Quad.gltf");
    ASSERT_NE(entry, nullptr);
    const RVX::Tools::AssetGUID guid = entry->guid;
    EXPECT_EQ(RVX::Tools::AssetType::Model, entry->type);
    EXPECT_TRUE(entry->isDirty);

    RVX::Tools::AssetPipeline pipeline;
    pipeline.RegisterImporter(std::make_unique<RVX::Tools::ModelImporter>());

    ASSERT_TRUE(database.ReimportAsset(guid, pipeline));
    const fs::path importedPath = importedRoot / "Meshes" / "Quad.rva";
    ASSERT_TRUE(fs::exists(importedPath));
    const std::string artifact = ReadBinaryFile(importedPath);
    EXPECT_NE(artifact.find("RVX_MODEL_PREBAKE_V1"), std::string::npos);
    const std::string meshArtifact =
        ReadBinaryFile(importedRoot / "Meshes" / "Quad.rvdeps" / "meshes.rva");
    EXPECT_NE(meshArtifact.find("mesh.0.hasTangents=1"), std::string::npos);

    const RVX::Tools::AssetEntry* afterImport = database.GetAsset(guid);
    ASSERT_NE(afterImport, nullptr);
    EXPECT_FALSE(afterImport->isDirty);
    EXPECT_NE(0u, afterImport->importedModTime);

    fs::remove_all(dir);
}

TEST_F(RenderHonestyValidationFixture, ResourceManagerLoadsCookedMeshArtifactWithLOD)
{
    fs::path dir = MakeTempDir("rvx_resource_manager_mesh_artifact");
    const fs::path source = dir / "Quad.gltf";
    const fs::path output = dir / "Quad.rva";
    WriteMinimalQuadGltf(source);

    RVX::Tools::MeshImporter importer;
    RVX::Tools::MeshImportOptions options;
    options.generateTangents = true;
    options.generateLODs = true;
    options.lodCount = 2;

    const RVX::Tools::ImportResult result = importer.Import(source, output, &options);
    ASSERT_TRUE(result.success) << result.error;

    RVX::Resource::ResourceManager manager;
    manager.Initialize();

    RVX::IResource* resource = manager.LoadResource(output.string());
    ASSERT_NE(resource, nullptr);
    EXPECT_EQ(resource->GetType(), RVX::ResourceType::Mesh);
    auto* meshResource = dynamic_cast<RVX::Resource::MeshResource*>(resource);
    ASSERT_NE(meshResource, nullptr);
    ASSERT_EQ(meshResource->GetLODCount(), 2u);

    std::shared_ptr<RVX::Mesh> lod0 = meshResource->GetLODMesh(0);
    ASSERT_NE(lod0, nullptr);
    EXPECT_EQ(lod0->name, "CookQuad");
    EXPECT_EQ(lod0->GetVertexCount(), 4u);
    EXPECT_EQ(lod0->GetIndexCount(), 6u);
    EXPECT_TRUE(lod0->HasAttribute(RVX::VertexBufferNames::Position));
    EXPECT_TRUE(lod0->HasAttribute(RVX::VertexBufferNames::Normal));
    EXPECT_TRUE(lod0->HasAttribute(RVX::VertexBufferNames::UV));
    EXPECT_TRUE(lod0->HasAttribute(RVX::VertexBufferNames::Tangent));
    EXPECT_EQ(lod0->GetIndexType(), RVX::IndexType::UInt32);

    std::shared_ptr<RVX::Mesh> lod1 = meshResource->GetLODMesh(1);
    ASSERT_NE(lod1, nullptr);
    EXPECT_EQ(lod1->GetVertexCount(), 3u);
    EXPECT_EQ(lod1->GetIndexCount(), 3u);
    EXPECT_EQ(lod1->GetIndexType(), RVX::IndexType::UInt32);
    EXPECT_TRUE(lod1->HasAttribute(RVX::VertexBufferNames::Tangent));

    manager.Shutdown();
    fs::remove_all(dir);
}

TEST_F(RenderHonestyValidationFixture, ShaderImporterWritesPrebakedShaderArtifact)
{
    fs::path dir = MakeTempDir("rvx_shader_importer_artifact");
    const fs::path source = dir / "Fullscreen.ps.hlsl";
    const fs::path output = dir / "Fullscreen.ps.rva";
    WriteTextFile(source, R"(
float4 main() : SV_Target
{
    return float4(1.0, 0.0, 1.0, 1.0);
}
)");

    RVX::Tools::ShaderImporter importer(
        RVX::Tests::CreateDeterministicShaderCompiler);
    RVX::Tools::ShaderImportOptions options;
    options.stage = RVX::RHIShaderStage::Pixel;
    options.targetBackend = RVX::RHIBackendType::DX12;
    options.enableOptimization = false;

    const RVX::Tools::ImportResult result = importer.Import(source, output, &options);
    ASSERT_TRUE(result.success) << result.error;
    ASSERT_TRUE(fs::exists(output));
    ASSERT_EQ(1u, result.outputPaths.size());
    EXPECT_EQ(output.string(), result.outputPaths[0]);

    const std::string artifact = ReadBinaryFile(output);
    EXPECT_NE(artifact.find("RVX_SHADER_PREBAKE_V1"), std::string::npos);
    EXPECT_NE(artifact.find("backend=DirectX 12"), std::string::npos);
    EXPECT_NE(artifact.find("stage=Pixel"), std::string::npos);
    EXPECT_NE(artifact.find("entry=main"), std::string::npos);
    EXPECT_NE(artifact.find("bytecodeSize="), std::string::npos);
    EXPECT_NE(artifact.find("reflectionResources="), std::string::npos);
    EXPECT_NE(artifact.find("RVX_SHADER_BYTECODE_BEGIN"), std::string::npos);
    EXPECT_NE(artifact.find("RVX_SHADER_PREBAKE_END"), std::string::npos);

    fs::remove_all(dir);
}

TEST_F(RenderHonestyValidationFixture, ResourceManagerLoadsCookedShaderArtifactByHeader)
{
    fs::path dir = MakeTempDir("rvx_resource_manager_shader_artifact");
    const fs::path output = dir / "Fullscreen.ps.rva";
    fs::create_directories(dir);

    const std::vector<uint8_t> bytecode = {0x44, 0x58, 0x42, 0x43, 0x00, 0x01, 0x02, 0x03};
    const std::string glsl = "#version 450\nvoid main() {}\n";
    const std::string msl = "fragment float4 main0() { return float4(1.0); }\n";

    {
        std::ofstream file(output, std::ios::binary);
        ASSERT_TRUE(file.is_open());
        file << "RVX_SHADER_PREBAKE_V1\n";
        file << "source=Shaders/Fullscreen.ps.hlsl\n";
        file << "backend=DirectX 12\n";
        file << "stage=Pixel\n";
        file << "entry=main\n";
        file << "targetProfile=ps_6_0\n";
        file << "bytecodeSize=" << bytecode.size() << "\n";
        file << "glslSize=" << glsl.size() << "\n";
        file << "mslSize=" << msl.size() << "\n";
        file << "reflectionResources=3\n";
        file << "sourceHash=123456789\n";
        file << "RVX_SHADER_BYTECODE_BEGIN\n";
        file.write(reinterpret_cast<const char*>(bytecode.data()),
                   static_cast<std::streamsize>(bytecode.size()));
        file << "\nRVX_SHADER_GLSL_BEGIN\n";
        file.write(glsl.data(), static_cast<std::streamsize>(glsl.size()));
        file << "\nRVX_SHADER_MSL_BEGIN\n";
        file.write(msl.data(), static_cast<std::streamsize>(msl.size()));
        file << "\nRVX_SHADER_PREBAKE_END\n";
        ASSERT_TRUE(file.good());
    }

    RVX::Resource::ResourceManager manager;
    manager.Initialize();

    RVX::IResource* resource = manager.LoadResource(output.string());
    ASSERT_NE(resource, nullptr);
    EXPECT_EQ(resource->GetType(), RVX::ResourceType::Shader);
    EXPECT_TRUE(resource->IsLoaded());

    auto* shaderResource = dynamic_cast<RVX::Resource::ShaderResource*>(resource);
    ASSERT_NE(shaderResource, nullptr);
    EXPECT_EQ(shaderResource->GetBackend(), RVX::Resource::ShaderBackendType::DX12);
    EXPECT_EQ(shaderResource->GetStage(), RVX::Resource::ShaderStage::Pixel);
    EXPECT_EQ(shaderResource->GetEntryPoint(), "main");
    EXPECT_EQ(shaderResource->GetTargetProfile(), "ps_6_0");
    EXPECT_EQ(shaderResource->GetSourceHash(), 123456789u);
    EXPECT_EQ(shaderResource->GetReflectionResourceCount(), 3u);
    EXPECT_EQ(shaderResource->GetMetadata().sourcePath, "Shaders/Fullscreen.ps.hlsl");
    EXPECT_EQ(shaderResource->GetBytecode(), bytecode);
    EXPECT_EQ(shaderResource->GetGLSLSource(), glsl);
    EXPECT_EQ(shaderResource->GetMSLSource(), msl);
    EXPECT_TRUE(shaderResource->HasBytecode());
    EXPECT_TRUE(shaderResource->HasTranslatedSource());

    manager.Shutdown();
    fs::remove_all(dir);
}

TEST_F(RenderHonestyValidationFixture, AssetDatabaseReimportAcceptsShaderImporterWhenArtifactIsWritten)
{
    fs::path dir = MakeTempDir("rvx_asset_database_shader_reimport");
    fs::path sourceRoot = dir / "Source";
    fs::path importedRoot = dir / "Imported";
    fs::create_directories(sourceRoot / "Shaders");
    const fs::path source = sourceRoot / "Shaders" / "Fullscreen.ps.hlsl";
    WriteTextFile(source, R"(
float4 main() : SV_Target
{
    return float4(0.25, 0.5, 1.0, 1.0);
}
)");

    RVX::Tools::AssetDatabase database;
    ASSERT_TRUE(database.Initialize(sourceRoot, importedRoot));
    const RVX::Tools::AssetEntry* entry = database.GetAssetByPath("Shaders/Fullscreen.ps.hlsl");
    ASSERT_NE(entry, nullptr);
    const RVX::Tools::AssetGUID guid = entry->guid;
    EXPECT_TRUE(entry->isDirty);

    RVX::Tools::AssetPipeline pipeline;
    pipeline.RegisterImporter(std::make_unique<RVX::Tools::ShaderImporter>(
        RVX::Tests::CreateDeterministicShaderCompiler));

    const bool imported = database.ReimportAsset(guid, pipeline);
    ASSERT_TRUE(imported);
    const fs::path importedPath = importedRoot / "Shaders" / "Fullscreen.ps.rva";
    ASSERT_TRUE(fs::exists(importedPath));
    const std::string artifact = ReadBinaryFile(importedPath);
    EXPECT_NE(artifact.find("RVX_SHADER_PREBAKE_V1"), std::string::npos);
    EXPECT_NE(artifact.find("stage=Pixel"), std::string::npos);

    const RVX::Tools::AssetEntry* afterImport = database.GetAsset(guid);
    ASSERT_NE(afterImport, nullptr);
    EXPECT_FALSE(afterImport->isDirty);
    EXPECT_NE(0u, afterImport->importedModTime);

    fs::remove_all(dir);
}

TEST_F(RenderHonestyValidationFixture, MaterialTemplateCompileDoesNotReportPlaceholderSuccess)
{
    NullDevice device;
    RVX::MaterialTemplate materialTemplate("honesty-test");
    materialTemplate.SetVertexShader("missing_vs.hlsl");
    materialTemplate.SetPixelShader("missing_ps.hlsl");

    EXPECT_FALSE(materialTemplate.Compile(&device));
    EXPECT_FALSE(materialTemplate.IsCompiled());
    EXPECT_FALSE(materialTemplate.GetLastCompileError().empty());
}

TEST_F(RenderHonestyValidationFixture, MaterialBinderRejectsInitializationWithoutDevice)
{
    RVX::MaterialBinder binder;
    binder.Initialize(nullptr, nullptr);

    EXPECT_FALSE(binder.IsInitialized());
    EXPECT_EQ(binder.GetLastBindStatus(), RVX::MaterialBindStatus::Error);
    EXPECT_FALSE(binder.GetLastBindMessage().empty());
}

TEST_F(RenderHonestyValidationFixture, GPUProfilerReportsTimestampQueriesUnavailable)
{
    NullDevice device;
    RVX::GPUProfiler profiler;
    profiler.Initialize(&device);

    EXPECT_TRUE(profiler.IsInitialized());
    EXPECT_FALSE(profiler.IsTimestampSupported());
    EXPECT_TRUE(profiler.GetResults().empty());
    EXPECT_EQ(profiler.GetStats().queryCount, 0u);
}

TEST_F(RenderHonestyValidationFixture, QueryCapabilitiesDefaultToUnsupported)
{
    RVX::RHICapabilities caps;

    EXPECT_FALSE(caps.supportsRaytracing);
    EXPECT_FALSE(caps.supportsRaytracingPipeline);
    EXPECT_FALSE(caps.supportsRayQuery);
    EXPECT_FALSE(caps.supportsAccelerationStructureUpdate);
    EXPECT_FALSE(caps.supportsAccelerationStructureCompaction);
    EXPECT_EQ(caps.maxRayRecursionDepth, 0u);
    EXPECT_EQ(caps.shaderGroupHandleSize, 0u);
    EXPECT_EQ(caps.shaderGroupHandleAlignment, 0u);
    EXPECT_EQ(caps.shaderTableBaseAlignment, 0u);
    EXPECT_FALSE(caps.supportsTimestampQueries);
    EXPECT_FALSE(caps.supportsOcclusionQueries);
    EXPECT_FALSE(caps.supportsPipelineStatisticsQueries);
    EXPECT_EQ(caps.timestampFrequency, 0u);
    EXPECT_FALSE(caps.supportsExplicitHeapManagement);
    EXPECT_FALSE(caps.supportsHostFenceSignal);
    EXPECT_FALSE(caps.supportsDefaultQueueFenceSignal);
    EXPECT_FALSE(caps.supportsExplicitQueueFenceSignal);
    EXPECT_FALSE(caps.supportsQueueFenceWait);
    EXPECT_FALSE(caps.supportsMultiQueueBatchSubmit);
    EXPECT_FALSE(caps.emulatesQueueFences);
    EXPECT_FALSE(caps.supportsDescriptorSets);
    EXPECT_FALSE(caps.supportsDynamicDescriptorOffsets);
    EXPECT_EQ(caps.maxDescriptorSets, 0u);
    EXPECT_FALSE(caps.supportsExplicitResourceBarriers);
    EXPECT_FALSE(caps.emulatesResourceBarriers);
}

TEST_F(RenderHonestyValidationFixture, QueryPoolDescriptionValidationRejectsInvalidContracts)
{
    RVX::RHIQueryPoolDesc desc;
    EXPECT_TRUE(RVX::ValidateRHIQueryPoolDesc(desc));

    desc.count = 0;
    RVX::RHIQueryValidationResult result = RVX::ValidateRHIQueryPoolDesc(desc);
    EXPECT_FALSE(result);
    EXPECT_STREQ(result.message, "query pool count must be greater than zero");

    desc.count = 1;
    desc.queueType = RVX::RHICommandQueueType::Compute;
    result = RVX::ValidateRHIQueryPoolDesc(desc);
    EXPECT_FALSE(result);
    EXPECT_STREQ(result.message,
                 "timestamp query pools currently require the Graphics queue");

    desc.queueType = RVX::RHICommandQueueType::Graphics;
    desc.type = static_cast<RVX::RHIQueryType>(255);
    result = RVX::ValidateRHIQueryPoolDesc(desc);
    EXPECT_FALSE(result);
    EXPECT_STREQ(result.message, "query pool type is invalid");
}

TEST_F(RenderHonestyValidationFixture, QueryRangeValidationRejectsOutOfBoundsResolves)
{
    class TestQueryPool final : public RVX::RHIQueryPool
    {
    public:
        TestQueryPool()
            : RHIQueryPool(RVX::RHICommandQueueType::Graphics, 64u)
        {
        }

        RVX::RHIQueryType GetType() const override { return RVX::RHIQueryType::Timestamp; }
        RVX::uint32 GetCount() const override { return 4; }
        RVX::uint64 GetTimestampFrequency() const override { return 1000000000ull; }
    };

    TestQueryPool pool;
    EXPECT_TRUE(RVX::ValidateRHIQueryPoolMetadata(
        pool, RVX::RHIQueryType::Timestamp,
        RVX::RHICommandQueueType::Graphics));
    RVX::RHIQueryValidationResult metadata =
        RVX::ValidateRHIQueryPoolMetadata(
            pool, RVX::RHIQueryType::Timestamp,
            RVX::RHICommandQueueType::Compute);
    EXPECT_FALSE(metadata);
    EXPECT_STREQ(metadata.message,
                 "query pool queue does not match the command context queue");
    EXPECT_TRUE(RVX::ValidateRHIQueryRange(pool, 0, 4));
    EXPECT_TRUE(RVX::ValidateRHIQueryRange(pool, 3, 1));

    RVX::RHIQueryValidationResult result = RVX::ValidateRHIQueryRange(pool, 0, 0);
    EXPECT_FALSE(result);
    EXPECT_STREQ(result.message, "query range count must be greater than zero");

    result = RVX::ValidateRHIQueryRange(pool, 4, 1);
    EXPECT_FALSE(result);
    EXPECT_STREQ(result.message, "query range exceeds query pool bounds");

    result = RVX::ValidateRHIQueryRange(pool, 2, 3);
    EXPECT_FALSE(result);
    EXPECT_STREQ(result.message, "query range exceeds query pool bounds");
}

TEST_F(RenderHonestyValidationFixture, RayTracingRHIContractDefaultsToUnsupported)
{
    NullDevice device;
    NoOpCommandContext ctx;

    RVX::RHIBottomLevelASDesc blasDesc;
    RVX::RHITopLevelASDesc tlasDesc;
    RVX::RHIAccelerationStructureDesc asDesc;
    RVX::RHIRayTracingPipelineDesc pipelineDesc;
    RVX::RHIShaderTableDesc shaderTableDesc;

    EXPECT_FALSE(RVX::ValidateRHIAccelerationStructureDesc(asDesc));
    asDesc.size = 4096;
    EXPECT_TRUE(RVX::ValidateRHIAccelerationStructureDesc(asDesc));

    RVX::RHIAccelerationStructureBuildSizes buildSizes;
    EXPECT_FALSE(buildSizes.IsValid());
    buildSizes.accelerationStructureSize = RVX::RVX_RAY_TRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT;
    buildSizes.buildScratchSize = RVX::RVX_RAY_TRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT;
    EXPECT_TRUE(buildSizes.IsValid());
    EXPECT_FALSE(buildSizes.HasValidUpdateScratchSize());
    buildSizes.updateScratchSize = RVX::RVX_RAY_TRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT;
    EXPECT_TRUE(buildSizes.HasValidUpdateScratchSize());
    buildSizes.accelerationStructureSize += 1;
    EXPECT_FALSE(buildSizes.IsValid());

    RVX::RHIAccelerationStructureDesc unalignedASDesc = asDesc;
    unalignedASDesc.size = RVX::RVX_RAY_TRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT + 1;
    EXPECT_FALSE(RVX::ValidateRHIAccelerationStructureDesc(unalignedASDesc));
    RVX::RHIAccelerationStructureDesc invalidASTypeDesc = asDesc;
    invalidASTypeDesc.type = static_cast<RVX::RHIAccelerationStructureType>(0xFFu);
    EXPECT_FALSE(RVX::ValidateRHIAccelerationStructureDesc(invalidASTypeDesc));

    EXPECT_FALSE(device.GetBottomLevelASBuildSizes(blasDesc).IsValid());
    EXPECT_FALSE(device.GetTopLevelASBuildSizes(tlasDesc).IsValid());
    EXPECT_EQ(device.CreateAccelerationStructure(asDesc).Get(), nullptr);
    EXPECT_EQ(device.CreateRayTracingPipeline(pipelineDesc).Get(), nullptr);
    EXPECT_EQ(device.CreateShaderTable(shaderTableDesc).Get(), nullptr);

    ctx.BuildBottomLevelAccelerationStructure(nullptr, blasDesc, nullptr);
    ctx.BuildTopLevelAccelerationStructure(nullptr, tlasDesc, nullptr);
    ctx.DispatchRays({});
}

TEST_F(RenderHonestyValidationFixture, RayTracedShadowPassDoesNotReportSupportBeforePipelineExists)
{
    RVX::RayTracedShadowPass pass;

    EXPECT_FALSE(pass.IsRequestedEnabled());
    EXPECT_FALSE(pass.IsSupported());
    EXPECT_FALSE(pass.IsEnabled());
    EXPECT_NE(pass.GetUnsupportedReason().find("not been requested"), std::string::npos);

    pass.SetEnabled(true);
    EXPECT_TRUE(pass.IsRequestedEnabled());
    EXPECT_FALSE(pass.IsSupported());
    EXPECT_FALSE(pass.IsEnabled());
    EXPECT_NE(pass.GetUnsupportedReason().find("RHI device"), std::string::npos);

    NullDevice device;
    pass.OnAdd(&device);
    EXPECT_FALSE(pass.IsSupported());
    EXPECT_FALSE(pass.IsEnabled());
    EXPECT_NE(pass.GetUnsupportedReason().find("does not support ray tracing"), std::string::npos);

    pass.OnRemove();
    EXPECT_FALSE(pass.IsRequestedEnabled());
}

TEST_F(RenderHonestyValidationFixture, RayTracingDescriptorValidationRequiresAccelerationStructure)
{
    FakeAccelerationStructure tlas(RVX::RHIAccelerationStructureType::TopLevel);
    FakeAccelerationStructure blas(RVX::RHIAccelerationStructureType::BottomLevel);
    FakeAccelerationStructure zeroAddressTlas(RVX::RHIAccelerationStructureType::TopLevel, 0);
    FakeBuffer buffer;

    FakeDescriptorSetLayout layout({
        RVX::RHIBindingLayoutEntry{0, RVX::RHIBindingType::AccelerationStructure, RVX::RHIShaderStage::AllRayTracing, 1, false}
    });

    RVX::RHIDescriptorSetDesc validDesc;
    validDesc.SetLayout(&layout).BindAccelerationStructure(0, &tlas);
    EXPECT_TRUE(RVX::ValidateRHIDescriptorSetDesc(validDesc));

    RVX::RHIDescriptorSetDesc wrongResourceDesc;
    wrongResourceDesc.SetLayout(&layout).BindBuffer(0, &buffer);
    EXPECT_FALSE(RVX::ValidateRHIDescriptorSetDesc(wrongResourceDesc));

    RVX::RHIDescriptorSetDesc missingResourceDesc;
    missingResourceDesc.SetLayout(&layout).BindAccelerationStructure(0, nullptr);
    EXPECT_FALSE(RVX::ValidateRHIDescriptorSetDesc(missingResourceDesc));

    RVX::RHIDescriptorSetDesc bottomLevelDesc;
    bottomLevelDesc.SetLayout(&layout).BindAccelerationStructure(0, &blas);
    EXPECT_FALSE(RVX::ValidateRHIDescriptorSetDesc(bottomLevelDesc));

    RVX::RHIDescriptorSetDesc zeroAddressDesc;
    zeroAddressDesc.SetLayout(&layout).BindAccelerationStructure(0, &zeroAddressTlas);
    EXPECT_FALSE(RVX::ValidateRHIDescriptorSetDesc(zeroAddressDesc));

    RVX::RHIDescriptorSetLayoutDesc dynamicLayout;
    dynamicLayout.AddDynamicBinding(0, RVX::RHIBindingType::AccelerationStructure);
    EXPECT_FALSE(RVX::ValidateRHIDescriptorSetLayoutDesc(dynamicLayout));
}

TEST_F(RenderHonestyValidationFixture, DescriptorValidationSupportsArrayElements)
{
    FakeBuffer firstBuffer;
    FakeBuffer secondBuffer;

    FakeDescriptorSetLayout layout({
        RVX::RHIBindingLayoutEntry{0, RVX::RHIBindingType::ShaderResourceBuffer, RVX::RHIShaderStage::AllRayTracing, 2, false}
    });

    RVX::RHIDescriptorSetDesc validDesc;
    validDesc.SetLayout(&layout)
        .BindBuffer(0, &firstBuffer, 0, RVX::RVX_WHOLE_SIZE, 0)
        .BindBuffer(0, &secondBuffer, 0, RVX::RVX_WHOLE_SIZE, 1);
    EXPECT_TRUE(RVX::ValidateRHIDescriptorSetDesc(validDesc));
    ASSERT_EQ(validDesc.bindings.size(), 2u);
    EXPECT_EQ(validDesc.bindings[0].arrayElement, 0u);
    EXPECT_EQ(validDesc.bindings[1].arrayElement, 1u);

    RVX::RHIDescriptorSetDesc duplicateElementDesc;
    duplicateElementDesc.SetLayout(&layout)
        .BindBuffer(0, &firstBuffer, 0, RVX::RVX_WHOLE_SIZE, 1)
        .BindBuffer(0, &secondBuffer, 0, RVX::RVX_WHOLE_SIZE, 1);
    EXPECT_FALSE(RVX::ValidateRHIDescriptorSetDesc(duplicateElementDesc));

    RVX::RHIDescriptorSetDesc outOfRangeDesc;
    outOfRangeDesc.SetLayout(&layout)
        .BindBuffer(0, &firstBuffer, 0, RVX::RVX_WHOLE_SIZE, 2);
    EXPECT_FALSE(RVX::ValidateRHIDescriptorSetDesc(outOfRangeDesc));
}

TEST_F(RenderHonestyValidationFixture, RayTracingDescriptionsValidateExplicitContracts)
{
    const RVX::RHIBufferUsage asInputUsage = RVX::RHIBufferUsage::AccelerationStructureInput |
                                             RVX::RHIBufferUsage::DeviceAddress;
    FakeBuffer vertexBuffer(asInputUsage);
    FakeBuffer indexBuffer(asInputUsage);
    FakeAccelerationStructure blas(RVX::RHIAccelerationStructureType::BottomLevel);
    FakeShader rayGen(RVX::RHIShaderStage::RayGeneration);
    FakeShader miss(RVX::RHIShaderStage::Miss);
    FakeShader closestHit(RVX::RHIShaderStage::ClosestHit);
    FakeShader pixelShader(RVX::RHIShaderStage::Pixel);
    FakePipelineLayout pipelineLayout;
    FakePipeline rtPipeline(true);
    RVX::RHIPipelineRef ownedRtPipeline(new FakePipeline(true));
    FakeShaderTable shaderTable(&rtPipeline);

    RVX::RHIBottomLevelASDesc emptyBlas;
    EXPECT_FALSE(RVX::ValidateRHIBottomLevelASDesc(emptyBlas));

    RVX::RHIRayTracingGeometryDesc geometry;
    geometry.triangles.vertexBuffer = &vertexBuffer;
    geometry.triangles.vertexStride = sizeof(float) * 3;
    geometry.triangles.vertexCount = 3;
    geometry.triangles.indexBuffer = &indexBuffer;
    geometry.triangles.indexCount = 3;
    geometry.triangles.indexFormat = RVX::RHIFormat::R32_UINT;

    RVX::RHIBottomLevelASDesc validBlas;
    validBlas.geometries.push_back(geometry);
    EXPECT_TRUE(RVX::ValidateRHIBottomLevelASDesc(validBlas));

    RVX::RHIRayTracingGeometryDesc invalidGeometryFlags = geometry;
    invalidGeometryFlags.flags = static_cast<RVX::RHIRayTracingGeometryFlags>(0x80000000u);
    EXPECT_FALSE(RVX::ValidateRHIRayTracingGeometryDesc(invalidGeometryFlags));

    RVX::RHIRayTracingGeometryDesc invalidGeometryType = geometry;
    invalidGeometryType.type = static_cast<RVX::RHIRayTracingGeometryType>(0xFFu);
    invalidGeometryType.aabbs.aabbBuffer = &vertexBuffer;
    invalidGeometryType.aabbs.count = 1;
    invalidGeometryType.aabbs.stride = 24;
    EXPECT_FALSE(RVX::ValidateRHIRayTracingGeometryDesc(invalidGeometryType));

    RVX::RHIBottomLevelASDesc invalidBlasBuildFlags = validBlas;
    invalidBlasBuildFlags.buildFlags = static_cast<RVX::RHIAccelerationStructureBuildFlags>(0x80000000u);
    EXPECT_FALSE(RVX::ValidateRHIBottomLevelASDesc(invalidBlasBuildFlags));

    RVX::RHIRayTracingGeometryDesc invalidIndexGeometry = geometry;
    invalidIndexGeometry.triangles.indexFormat = RVX::RHIFormat::RGBA8_UNORM;
    RVX::RHIBottomLevelASDesc invalidIndexBlas;
    invalidIndexBlas.geometries.push_back(invalidIndexGeometry);
    EXPECT_FALSE(RVX::ValidateRHIBottomLevelASDesc(invalidIndexBlas));

    invalidIndexGeometry.triangles.indexFormat = RVX::RHIFormat::R8_UINT;
    invalidIndexBlas.geometries.clear();
    invalidIndexBlas.geometries.push_back(invalidIndexGeometry);
    EXPECT_FALSE(RVX::ValidateRHIBottomLevelASDesc(invalidIndexBlas));

    RVX::RHITopLevelASDesc validTlas;
    RVX::RHIRayTracingInstanceDesc instance;
    instance.bottomLevel = &blas;
    validTlas.instances.push_back(instance);
    EXPECT_TRUE(RVX::ValidateRHITopLevelASDesc(validTlas));

    RVX::RHITopLevelASDesc invalidTlasBuildFlags = validTlas;
    invalidTlasBuildFlags.buildFlags = static_cast<RVX::RHIAccelerationStructureBuildFlags>(0x80000000u);
    EXPECT_FALSE(RVX::ValidateRHITopLevelASDesc(invalidTlasBuildFlags));

    RVX::RHITopLevelASDesc validBufferedTlas;
    validBufferedTlas.instanceBuffer = &vertexBuffer;
    validBufferedTlas.instanceCount = 1;
    EXPECT_TRUE(RVX::ValidateRHITopLevelASDesc(validBufferedTlas));

    RVX::RHITopLevelASDesc mixedInstanceSourceTlas = validBufferedTlas;
    mixedInstanceSourceTlas.instances.push_back(instance);
    EXPECT_FALSE(RVX::ValidateRHITopLevelASDesc(mixedInstanceSourceTlas));

    RVX::RHITopLevelASDesc unalignedBufferedTlas = validBufferedTlas;
    unalignedBufferedTlas.instanceOffset = RVX::RVX_RAY_TRACING_INSTANCE_DESC_ALIGNMENT / 2;
    EXPECT_FALSE(RVX::ValidateRHITopLevelASDesc(unalignedBufferedTlas));

    RVX::RHITopLevelASDesc alignedBufferedTlas = validBufferedTlas;
    alignedBufferedTlas.instanceOffset = RVX::RVX_RAY_TRACING_INSTANCE_DESC_ALIGNMENT;
    EXPECT_TRUE(RVX::ValidateRHITopLevelASDesc(alignedBufferedTlas));

    RVX::RHITopLevelASDesc invalidTlas;
    invalidTlas.instances.push_back({});
    EXPECT_FALSE(RVX::ValidateRHITopLevelASDesc(invalidTlas));

    RVX::RHITopLevelASDesc overflowInstanceIdTlas = validTlas;
    overflowInstanceIdTlas.instances[0].instanceId = RVX::RVX_RAY_TRACING_INSTANCE_METADATA_MASK + 1u;
    EXPECT_FALSE(RVX::ValidateRHITopLevelASDesc(overflowInstanceIdTlas));

    RVX::RHITopLevelASDesc overflowInstanceMaskTlas = validTlas;
    overflowInstanceMaskTlas.instances[0].instanceMask = RVX::RVX_RAY_TRACING_INSTANCE_MASK_MASK + 1u;
    EXPECT_FALSE(RVX::ValidateRHITopLevelASDesc(overflowInstanceMaskTlas));

    RVX::RHITopLevelASDesc overflowContributionTlas = validTlas;
    overflowContributionTlas.instances[0].instanceContributionToHitGroupIndex =
        RVX::RVX_RAY_TRACING_INSTANCE_METADATA_MASK + 1u;
    EXPECT_FALSE(RVX::ValidateRHITopLevelASDesc(overflowContributionTlas));

    RVX::RHITopLevelASDesc unknownFlagsTlas = validTlas;
    unknownFlagsTlas.instances[0].flags =
        static_cast<RVX::RHIRayTracingInstanceFlags>(RVX::RVX_RAY_TRACING_INSTANCE_FLAGS_MASK + 1u);
    EXPECT_FALSE(RVX::ValidateRHITopLevelASDesc(unknownFlagsTlas));

    RVX::RHITopLevelASDesc invalidBufferedTlas;
    invalidBufferedTlas.instanceBuffer = &vertexBuffer;
    EXPECT_FALSE(RVX::ValidateRHITopLevelASDesc(invalidBufferedTlas));

    RVX::RHIRayTracingPipelineDesc pipelineDesc;
    pipelineDesc.pipelineLayout = &pipelineLayout;
    pipelineDesc.shaderGroups.push_back({RVX::RHIRayTracingShaderGroupType::General, "RayGen", &rayGen});
    pipelineDesc.shaderGroups.push_back({RVX::RHIRayTracingShaderGroupType::General, "Miss", &miss});
    pipelineDesc.shaderGroups.push_back({RVX::RHIRayTracingShaderGroupType::TrianglesHitGroup, "Hit", nullptr, &closestHit});
    EXPECT_TRUE(RVX::ValidateRHIRayTracingPipelineDesc(pipelineDesc));

    RVX::RHIRayTracingPipelineDesc invalidPipelineDesc = pipelineDesc;
    invalidPipelineDesc.shaderGroups[0].generalShader = &pixelShader;
    EXPECT_FALSE(RVX::ValidateRHIRayTracingPipelineDesc(invalidPipelineDesc));

    RVX::RHIShaderTableDesc shaderTableDesc;
    shaderTableDesc.rayTracingPipeline = &rtPipeline;
    shaderTableDesc.rayGenerationRecords.push_back({0});
    shaderTableDesc.missRecords.push_back({1});
    shaderTableDesc.hitGroupRecords.push_back({2});
    shaderTableDesc.callableRecords.push_back({3});
    EXPECT_TRUE(RVX::ValidateRHIShaderTableDesc(shaderTableDesc));

    RVX::RHIShaderTableDesc ownerShaderTableDesc = shaderTableDesc;
    ownerShaderTableDesc.rayTracingPipeline = nullptr;
    ownerShaderTableDesc.rayTracingPipelineOwner = ownedRtPipeline;
    EXPECT_TRUE(RVX::ValidateRHIShaderTableDesc(ownerShaderTableDesc));

    RVX::RHIShaderTableDesc mismatchedPipelineOwnerDesc = shaderTableDesc;
    mismatchedPipelineOwnerDesc.rayTracingPipelineOwner = ownedRtPipeline;
    EXPECT_FALSE(RVX::ValidateRHIShaderTableDesc(mismatchedPipelineOwnerDesc));

    RVX::RHIShaderTableDesc wrongRayGenRecordDesc = shaderTableDesc;
    wrongRayGenRecordDesc.rayGenerationRecords[0].shaderGroupIndex = 1;
    EXPECT_FALSE(RVX::ValidateRHIShaderTableDesc(wrongRayGenRecordDesc));

    RVX::RHIShaderTableDesc wrongMissRecordDesc = shaderTableDesc;
    wrongMissRecordDesc.missRecords[0].shaderGroupIndex = 0;
    EXPECT_FALSE(RVX::ValidateRHIShaderTableDesc(wrongMissRecordDesc));

    RVX::RHIShaderTableDesc wrongHitGroupRecordDesc = shaderTableDesc;
    wrongHitGroupRecordDesc.hitGroupRecords[0].shaderGroupIndex = 0;
    EXPECT_FALSE(RVX::ValidateRHIShaderTableDesc(wrongHitGroupRecordDesc));

    RVX::RHIShaderTableDesc wrongCallableRecordDesc = shaderTableDesc;
    wrongCallableRecordDesc.callableRecords[0].shaderGroupIndex = 1;
    EXPECT_FALSE(RVX::ValidateRHIShaderTableDesc(wrongCallableRecordDesc));

    RVX::RHIShaderTableDesc outOfRangeRecordDesc = shaderTableDesc;
    outOfRangeRecordDesc.missRecords[0].shaderGroupIndex = 4;
    EXPECT_FALSE(RVX::ValidateRHIShaderTableDesc(outOfRangeRecordDesc));

    RVX::uint32 localRootConstants = 0x12345678u;
    RVX::RHIShaderTableDesc localRootShaderTableDesc = shaderTableDesc;
    localRootShaderTableDesc.hitGroupRecords[0] = {2, &localRootConstants, sizeof(localRootConstants)};
    EXPECT_FALSE(RVX::ValidateRHIShaderTableDesc(localRootShaderTableDesc));

    RVX::RHIShaderTableDesc danglingLocalRootPointerDesc = shaderTableDesc;
    danglingLocalRootPointerDesc.hitGroupRecords[0].localRootData = &localRootConstants;
    EXPECT_FALSE(RVX::ValidateRHIShaderTableDesc(danglingLocalRootPointerDesc));

    RVX::RHIShaderTableDesc missingLocalRootDataDesc = shaderTableDesc;
    missingLocalRootDataDesc.hitGroupRecords[0].localRootDataSize = sizeof(localRootConstants);
    EXPECT_FALSE(RVX::ValidateRHIShaderTableDesc(missingLocalRootDataDesc));

    FakePipeline graphicsPipeline(false);
    RVX::RHIShaderTableDesc nonRayTracingShaderTableDesc = shaderTableDesc;
    nonRayTracingShaderTableDesc.rayTracingPipeline = &graphicsPipeline;
    EXPECT_FALSE(RVX::ValidateRHIShaderTableDesc(nonRayTracingShaderTableDesc));

    shaderTableDesc.rayGenerationRecords.push_back({0});
    EXPECT_FALSE(RVX::ValidateRHIShaderTableDesc(shaderTableDesc));

    FakeShaderTable missingRayGenShaderTable(&rtPipeline, 0);
    FakeShaderTable duplicateRayGenShaderTable(&rtPipeline, 2);
    RVX::uint64 dispatchRayCount = 0;
    EXPECT_TRUE(RVX::TryGetRHIRayTracingDispatchRayCount(128, 64, 2, dispatchRayCount));
    EXPECT_EQ(dispatchRayCount, 128ull * 64ull * 2ull);
    const RVX::uint32 maxUint32 = ~static_cast<RVX::uint32>(0);
    EXPECT_FALSE(RVX::TryGetRHIRayTracingDispatchRayCount(maxUint32, maxUint32, 2, dispatchRayCount));
    EXPECT_TRUE(RVX::ValidateRHIDispatchRaysDesc({&shaderTable, 128, 64, 1}));
    EXPECT_TRUE(RVX::ValidateRHIDispatchRaysDesc({&shaderTable, 128, 64, 1}, &rtPipeline));
    RVX::RHIPipelineRef secondOwnedRtPipeline(new FakePipeline(true));
    EXPECT_FALSE(RVX::ValidateRHIDispatchRaysDesc({&shaderTable, 128, 64, 1}, secondOwnedRtPipeline.Get()));
    EXPECT_FALSE(RVX::ValidateRHIDispatchRaysDesc({&shaderTable, 128, 64, 1}, &graphicsPipeline));
    FakeShaderTable unownedShaderTable;
    EXPECT_FALSE(RVX::ValidateRHIDispatchRaysDesc({&unownedShaderTable, 128, 64, 1}));
    EXPECT_FALSE(RVX::ValidateRHIDispatchRaysDesc({&shaderTable, 0, 64, 1}));
    EXPECT_FALSE(RVX::ValidateRHIDispatchRaysDesc({&shaderTable, maxUint32, maxUint32, 2}));
    EXPECT_FALSE(RVX::ValidateRHIDispatchRaysDesc({nullptr, 128, 64, 1}));
    EXPECT_FALSE(RVX::ValidateRHIDispatchRaysDesc({&missingRayGenShaderTable, 128, 64, 1}));
    EXPECT_FALSE(RVX::ValidateRHIDispatchRaysDesc({&duplicateRayGenShaderTable, 128, 64, 1}));
}

TEST_F(RenderHonestyValidationFixture, DescriptorValidationHelpersRejectInvalidInputs)
{
    RVX::RHIDescriptorSetLayoutDesc duplicateLayout;
    duplicateLayout.AddBinding(0, RVX::RHIBindingType::UniformBuffer);
    duplicateLayout.AddBinding(0, RVX::RHIBindingType::Sampler);
    EXPECT_FALSE(RVX::ValidateRHIDescriptorSetLayoutDesc(duplicateLayout));

    RVX::RHIDescriptorSetLayoutDesc invalidDynamicLayout;
    invalidDynamicLayout.AddDynamicBinding(1, RVX::RHIBindingType::SampledTexture);
    EXPECT_FALSE(RVX::ValidateRHIDescriptorSetLayoutDesc(invalidDynamicLayout));

    RVX::RHIPipelineLayoutDesc invalidPipelineLayout;
    invalidPipelineLayout.setLayouts.push_back(nullptr);
    EXPECT_FALSE(RVX::ValidateRHIPipelineLayoutDesc(invalidPipelineLayout));

    RVX::RHIDescriptorSetDesc nullSetDesc;
    EXPECT_FALSE(RVX::ValidateRHIDescriptorSetDesc(nullSetDesc));
}

TEST_F(RenderHonestyValidationFixture, TextureReferenceFallbackIsObservable)
{
    RVX::Resource::TextureLoader loader(nullptr);
    RVX::Resource::TextureReference invalidReference;

    RVX::Resource::TextureResource* texture = loader.LoadFromReference(invalidReference, "model.gltf");

    ASSERT_NE(texture, nullptr);
    EXPECT_TRUE(texture->IsDefaultFallback());
    EXPECT_FALSE(texture->GetFallbackReason().empty());
    EXPECT_TRUE(loader.WasLastLoadFallback());
    EXPECT_EQ(loader.GetLastLoadStatus(), RVX::Resource::TextureLoadStatus::FallbackInvalidReference);
    EXPECT_FALSE(loader.GetLastLoadError().empty());
}

TEST_F(RenderHonestyValidationFixture, TextureCacheHitsReportLoadedStatus)
{
    RVX::Resource::ResourceManager manager;
    RVX::Resource::ResourceManagerConfig config;
    config.asyncThreadCount = 0;
    manager.Initialize(config);

    RVX::Resource::TextureLoader loader(&manager);
    std::vector<RVX::uint8> rgba = { 255, 255, 255, 255 };
    const fs::path cachePath = MakeTempDir("rvx_texture_cache") / "cached.png";

    RVX::Resource::TextureResource* first = loader.LoadFromMemory(
        rgba.data(), rgba.size(), cachePath.string(), RVX::Resource::TextureUsage::Color, true, 1, 1);
    ASSERT_NE(first, nullptr);
    EXPECT_FALSE(first->IsDefaultFallback());
    EXPECT_EQ(loader.GetLastLoadStatus(), RVX::Resource::TextureLoadStatus::Loaded);

    RVX::Resource::TextureReference invalidReference;
    RVX::Resource::TextureResource* invalidFallback = loader.LoadFromReference(invalidReference, "model.gltf");
    ASSERT_NE(invalidFallback, nullptr);
    EXPECT_TRUE(invalidFallback->IsDefaultFallback());
    ASSERT_TRUE(loader.WasLastLoadFallback());

    RVX::Resource::TextureResource* cachedMemory = loader.LoadFromMemory(
        rgba.data(), rgba.size(), cachePath.string(), RVX::Resource::TextureUsage::Color, true, 1, 1);
    EXPECT_EQ(cachedMemory, first);
    EXPECT_FALSE(cachedMemory->IsDefaultFallback());
    EXPECT_EQ(loader.GetLastLoadStatus(), RVX::Resource::TextureLoadStatus::Loaded);
    EXPECT_FALSE(loader.WasLastLoadFallback());
    EXPECT_TRUE(loader.GetLastLoadError().empty());

    ASSERT_NE(loader.LoadFromReference(invalidReference, "model.gltf"), nullptr);
    ASSERT_TRUE(loader.WasLastLoadFallback());

    RVX::Resource::TextureResource* cachedFile = loader.LoadFromFile(cachePath.string());
    EXPECT_EQ(cachedFile, first);
    EXPECT_FALSE(cachedFile->IsDefaultFallback());
    EXPECT_EQ(loader.GetLastLoadStatus(), RVX::Resource::TextureLoadStatus::Loaded);
    EXPECT_FALSE(loader.WasLastLoadFallback());
    EXPECT_TRUE(loader.GetLastLoadError().empty());

    std::vector<RVX::uint8> embeddedPixels = { 128, 128, 255, 255 };
    RVX::Resource::TextureReference embedded = RVX::Resource::TextureReference::CreateEmbeddedRaw(
        std::move(embeddedPixels), 7, 1, 1, RVX::Resource::TextureUsage::Normal, false);

    RVX::Resource::TextureResource* embeddedFirst = loader.LoadFromReference(embedded, "model.gltf");
    ASSERT_NE(embeddedFirst, nullptr);
    EXPECT_FALSE(embeddedFirst->IsDefaultFallback());
    EXPECT_EQ(loader.GetLastLoadStatus(), RVX::Resource::TextureLoadStatus::Loaded);

    ASSERT_NE(loader.LoadFromReference(invalidReference, "model.gltf"), nullptr);
    ASSERT_TRUE(loader.WasLastLoadFallback());

    RVX::Resource::TextureResource* embeddedCached = loader.LoadFromReference(embedded, "model.gltf");
    EXPECT_EQ(embeddedCached, embeddedFirst);
    EXPECT_FALSE(embeddedCached->IsDefaultFallback());
    EXPECT_EQ(loader.GetLastLoadStatus(), RVX::Resource::TextureLoadStatus::Loaded);
    EXPECT_FALSE(loader.WasLastLoadFallback());
    EXPECT_TRUE(loader.GetLastLoadError().empty());

    manager.Shutdown();
    fs::remove_all(cachePath.parent_path());
}

TEST_F(RenderHonestyValidationFixture, TextureLoaderGeneratesOrdinaryRgbaMipChain)
{
    std::vector<RVX::uint8> rgba(static_cast<size_t>(4 * 4 * 4), 255u);

    RVX::Resource::TextureLoader loader(nullptr);
    RVX::Resource::TextureResource* texture =
        loader.LoadFromMemory(rgba.data(), rgba.size(), "ordinary_4x4_rgba", RVX::Resource::TextureUsage::Color, true, 4, 4);

    ASSERT_NE(texture, nullptr);
    EXPECT_FALSE(texture->IsDefaultFallback());
    EXPECT_EQ(texture->GetMipLevels(), 3u);
    EXPECT_EQ(texture->GetData().size(), static_cast<size_t>(4 * 4 * 4 + 2 * 2 * 4 + 1 * 1 * 4));
}

TEST_F(RenderHonestyValidationFixture, TextureLoaderGeneratesColorMipsInLinearSpace)
{
    const std::vector<RVX::uint8> rgba = {
        0, 0, 0, 0,       255, 255, 255, 255,
        0, 0, 0, 255,     255, 255, 255, 255,
    };

    RVX::Resource::TextureLoader loader(nullptr);
    RVX::Resource::TextureResource* texture =
        loader.LoadFromMemory(rgba.data(), rgba.size(), "color_srgb_2x2", RVX::Resource::TextureUsage::Color, true, 2, 2);

    ASSERT_NE(texture, nullptr);
    ASSERT_EQ(texture->GetMipLevels(), 2u);
    const size_t mip1 = MipOffset(2, 2, 1);
    ASSERT_GE(texture->GetData().size(), mip1 + 4u);

    EXPECT_EQ(texture->GetData()[mip1 + 0], 188u);
    EXPECT_EQ(texture->GetData()[mip1 + 1], 188u);
    EXPECT_EQ(texture->GetData()[mip1 + 2], 188u);
    EXPECT_EQ(texture->GetData()[mip1 + 3], 191u);
}

TEST_F(RenderHonestyValidationFixture, TextureLoaderGeneratesNormalMipsInSignedSpace)
{
    const std::vector<RVX::uint8> rgba = {
        255, 128, 128, 255,   128, 128, 255, 255,
        128, 128, 255, 255,   128, 128, 255, 255,
    };

    RVX::Resource::TextureLoader loader(nullptr);
    RVX::Resource::TextureResource* texture =
        loader.LoadFromMemory(rgba.data(), rgba.size(), "normal_2x2", RVX::Resource::TextureUsage::Normal, true, 2, 2);

    ASSERT_NE(texture, nullptr);
    ASSERT_EQ(texture->GetMipLevels(), 2u);
    EXPECT_FALSE(texture->IsSRGB());
    const size_t mip1 = MipOffset(2, 2, 1);
    ASSERT_GE(texture->GetData().size(), mip1 + 4u);

    EXPECT_GE(texture->GetData()[mip1 + 0], 166u);
    EXPECT_LE(texture->GetData()[mip1 + 0], 170u);
    EXPECT_GE(texture->GetData()[mip1 + 2], 248u);
    EXPECT_EQ(texture->GetData()[mip1 + 3], 255u);
}

TEST_F(RenderHonestyValidationFixture, TextureLoaderGeneratesDataMipsWithByteLinearAverage)
{
    const std::vector<RVX::uint8> rgba = {
        0, 0, 0, 0,       255, 255, 255, 255,
        0, 0, 0, 255,     255, 255, 255, 255,
    };

    RVX::Resource::TextureLoader loader(nullptr);
    RVX::Resource::TextureResource* texture =
        loader.LoadFromMemory(rgba.data(), rgba.size(), "data_2x2", RVX::Resource::TextureUsage::Data, true, 2, 2);

    ASSERT_NE(texture, nullptr);
    ASSERT_EQ(texture->GetMipLevels(), 2u);
    EXPECT_FALSE(texture->IsSRGB());
    const size_t mip1 = MipOffset(2, 2, 1);
    ASSERT_GE(texture->GetData().size(), mip1 + 4u);

    EXPECT_EQ(texture->GetData()[mip1 + 0], 128u);
    EXPECT_EQ(texture->GetData()[mip1 + 1], 128u);
    EXPECT_EQ(texture->GetData()[mip1 + 2], 128u);
    EXPECT_EQ(texture->GetData()[mip1 + 3], 191u);
}

TEST_F(RenderHonestyValidationFixture, TextureReferenceExternalUsageControlsMipPolicyAndCacheIdentity)
{
    fs::path temp = MakeTempDir("rvx_texture_external_policy");
    const fs::path texturePath = temp / "shared_texture.tga";
    const fs::path modelPath = temp / "model.gltf";
    WriteTextFile(modelPath, "{}");

    const std::vector<RVX::uint8> rgba = {
        255, 128, 128, 255,   128, 128, 255, 255,
        128, 128, 255, 255,   128, 128, 255, 255,
    };
    WriteRgbaTga(texturePath, 2, 2, rgba);

    RVX::Resource::ResourceManager manager;
    RVX::Resource::ResourceManagerConfig config;
    config.asyncThreadCount = 0;
    manager.Initialize(config);

    RVX::Resource::TextureLoader loader(&manager);
    RVX::Resource::TextureReference colorRef =
        RVX::Resource::TextureReference::CreateExternal(texturePath.string(), RVX::Resource::TextureUsage::Color, true);
    RVX::Resource::TextureReference normalRef =
        RVX::Resource::TextureReference::CreateExternal(texturePath.string(), RVX::Resource::TextureUsage::Normal, false);

    RVX::Resource::TextureResource* color = loader.LoadFromReference(colorRef, modelPath.string());
    ASSERT_NE(color, nullptr);
    EXPECT_EQ(color->GetUsage(), RVX::Resource::TextureUsage::Color);
    EXPECT_TRUE(color->IsSRGB());

    RVX::Resource::TextureResource* normal = loader.LoadFromReference(normalRef, modelPath.string());
    ASSERT_NE(normal, nullptr);
    EXPECT_EQ(normal->GetUsage(), RVX::Resource::TextureUsage::Normal);
    EXPECT_FALSE(normal->IsSRGB());
    EXPECT_NE(color->GetId(), normal->GetId());
    EXPECT_NE(color, normal);

    const size_t mip1 = MipOffset(2, 2, 1);
    ASSERT_GE(normal->GetData().size(), mip1 + 4u);
    EXPECT_GE(normal->GetData()[mip1 + 2], 248u);

    manager.Shutdown();
    fs::remove_all(temp);
}

TEST_F(RenderHonestyValidationFixture, ResourceManagerTextureLoadDoesNotLeavePolicyCacheAlias)
{
    fs::path temp = MakeTempDir("rvx_texture_manager_policy_alias");
    const fs::path texturePath = temp / "manager_texture.tga";
    const std::vector<RVX::uint8> rgba = {
        0, 0, 0, 255,       255, 255, 255, 255,
        0, 0, 0, 255,       255, 255, 255, 255,
    };
    WriteRgbaTga(texturePath, 2, 2, rgba);

    RVX::Resource::ResourceManager manager;
    RVX::Resource::ResourceManagerConfig config;
    config.asyncThreadCount = 0;
    manager.Initialize(config);

    RVX::IResource* resource = manager.LoadResource(texturePath.string());
    ASSERT_NE(resource, nullptr);
    EXPECT_EQ(resource->GetType(), RVX::ResourceType::Texture);
    EXPECT_EQ(resource->GetId(), RVX::GenerateResourceId(texturePath.string()));
    EXPECT_TRUE(manager.IsLoaded(texturePath.string()));

    RVX::Resource::ResourceCache::Stats stats = manager.GetCache().GetStats();
    EXPECT_EQ(stats.totalResources, 1u);

    EXPECT_EQ(manager.Unload(resource->GetId()),
              RVX::Resource::AssetResidencyReleaseResult::Unloaded);
    EXPECT_FALSE(manager.IsLoaded(texturePath.string()));
    stats = manager.GetCache().GetStats();
    EXPECT_EQ(stats.totalResources, 0u);

    manager.Shutdown();
    fs::remove_all(temp);
}

TEST_F(RenderHonestyValidationFixture, ResourceManagerTextureLoadDoesNotStealExistingPolicyTexture)
{
    fs::path temp = MakeTempDir("rvx_texture_manager_policy_existing");
    const fs::path texturePath = temp / "shared_texture.tga";
    const fs::path modelPath = temp / "model.gltf";
    WriteTextFile(modelPath, "{}");

    const std::vector<RVX::uint8> rgba = {
        255, 128, 128, 255,   128, 128, 255, 255,
        128, 128, 255, 255,   128, 128, 255, 255,
    };
    WriteRgbaTga(texturePath, 2, 2, rgba);

    RVX::Resource::ResourceManager manager;
    RVX::Resource::ResourceManagerConfig config;
    config.asyncThreadCount = 0;
    manager.Initialize(config);

    RVX::Resource::TextureLoader loader(&manager);
    RVX::Resource::TextureReference normalRef =
        RVX::Resource::TextureReference::CreateExternal(texturePath.string(), RVX::Resource::TextureUsage::Normal, false);

    RVX::Resource::TextureResource* policyTexture = loader.LoadFromReference(normalRef, modelPath.string());
    ASSERT_NE(policyTexture, nullptr);
    const RVX::Resource::ResourceId policyId = policyTexture->GetId();
    EXPECT_TRUE(manager.GetCache().Contains(policyId));

    RVX::IResource* genericResource = manager.LoadResource(texturePath.string());
    ASSERT_NE(genericResource, nullptr);
    EXPECT_NE(genericResource, policyTexture);
    EXPECT_EQ(policyTexture->GetId(), policyId);
    EXPECT_TRUE(manager.GetCache().Contains(policyId));
    EXPECT_TRUE(manager.GetCache().Contains(RVX::GenerateResourceId(texturePath.string())));

    RVX::Resource::ResourceCache::Stats stats = manager.GetCache().GetStats();
    EXPECT_EQ(stats.totalResources, 2u);

    manager.Unload(texturePath.string());
    EXPECT_TRUE(manager.GetCache().Contains(policyId));
    EXPECT_EQ(policyTexture->GetId(), policyId);

    RVX::Resource::TextureResource* policyCached = loader.LoadFromReference(normalRef, modelPath.string());
    EXPECT_EQ(policyCached, policyTexture);
    EXPECT_EQ(policyCached->GetId(), policyId);

    manager.Shutdown();
    fs::remove_all(temp);
}

TEST_F(RenderHonestyValidationFixture, PostProcessStubPassesAreUnsupportedAndDisabled)
{
    RVX::PostProcessSettings settings;
    settings.enableDOF = true;
    settings.enableMotionBlur = true;
    settings.enableVolumetricLighting = true;

    RVX::DOFPass dof;
    RVX::MotionBlurPass motionBlur;
    RVX::VolumetricLightingPass volumetricLighting;

    RVX::IPostProcessPass* passes[] = {
        &dof,
        &motionBlur,
        &volumetricLighting,
    };

    for (RVX::IPostProcessPass* pass : passes)
    {
        pass->Configure(settings);
        EXPECT_TRUE(pass->IsRequestedEnabled()) << pass->GetName();
        EXPECT_FALSE(pass->IsSupported()) << pass->GetName();
        EXPECT_FALSE(pass->IsEnabled()) << pass->GetName();
        EXPECT_FALSE(pass->GetUnsupportedReason().empty()) << pass->GetName();
    }
}

TEST_F(RenderHonestyValidationFixture, FilmGrainRequiresResourcesBeforeSupported)
{
    RVX::PostProcessSettings settings;
    settings.enableFilmGrain = true;

    RVX::FilmGrainPass filmGrain;
    filmGrain.Configure(settings);

    EXPECT_TRUE(filmGrain.IsRequestedEnabled());
    EXPECT_FALSE(filmGrain.IsSupported());
    EXPECT_FALSE(filmGrain.IsEnabled());
    EXPECT_FALSE(filmGrain.GetUnsupportedReason().empty());
}

TEST_F(RenderHonestyValidationFixture, TerrainMaterialRenderDataExportDoesNotPretendGpuUpload)
{
    RVX::TerrainMaterial material;

    EXPECT_EQ(material.AddLayer("Grass", "terrain/grass_albedo", "terrain/grass_normal", 12.0f), 0u);

    std::vector<RVX::TerrainLayerRenderData> layerData;
    material.BuildLayerRenderData(layerData);

    ASSERT_EQ(layerData.size(), RVX::RVX_TERRAIN_MAX_LAYERS);
    EXPECT_FLOAT_EQ(layerData[0].tilingAndStrength.x, 12.0f);
    EXPECT_FALSE(material.IsGPUInitialized());
    EXPECT_FALSE(material.IsLayerBufferDataUploaded());
    EXPECT_NE(material.GetLayerBufferDiagnostic().find("Render-owned"), std::string::npos);
}

TEST_F(RenderHonestyValidationFixture, TerrainHeightmapGpuTextureUploadIsHonestWhenUnavailable)
{
    const float heights[] = {0.0f, 0.25f, 0.5f, 1.0f};
    RVX::HeightmapDesc desc;
    desc.width = 2;
    desc.height = 2;
    desc.format = RVX::HeightmapFormat::Float32;
    desc.initialData = heights;

    RVX::Heightmap heightmap;
    ASSERT_TRUE(heightmap.Create(desc));

    EXPECT_FALSE(heightmap.CreateGPUTexture());
    EXPECT_FALSE(heightmap.IsGPUTextureDataUploaded());
    EXPECT_NE(heightmap.GetGPUTextureDiagnostic().find("Render-owned"), std::string::npos);

    EXPECT_FALSE(heightmap.GenerateNormalMap(RVX::Vec3(1.0f)));
    EXPECT_FALSE(heightmap.IsNormalMapDataUploaded());
    EXPECT_NE(heightmap.GetNormalMapDiagnostic().find("Render-owned"), std::string::npos);
}

TEST_F(RenderHonestyValidationFixture, TerrainLODFallbacksExposeDeterministicStatus)
{
    const float heights[] = {0.0f, 0.25f, 0.5f, 1.0f};
    RVX::HeightmapDesc desc;
    desc.width = 2;
    desc.height = 2;
    desc.format = RVX::HeightmapFormat::Float32;
    desc.initialData = heights;

    RVX::Heightmap heightmap;
    ASSERT_TRUE(heightmap.Create(desc));

    RVX::TerrainLODParams params;
    params.maxLODLevels = 2;
    params.patchSize = 4;

    RVX::TerrainLOD lod;
    ASSERT_TRUE(lod.Initialize(&heightmap, RVX::Vec3(16.0f, 4.0f, 16.0f), params));
    EXPECT_TRUE(lod.UsesConservativeHeightBounds());
    EXPECT_NE(lod.GetHeightBoundsDiagnostic().find("conservative fallback"), std::string::npos);
    EXPECT_FALSE(lod.SupportsCrackPrevention());
    EXPECT_FALSE(lod.GetCrackPreventionDiagnostic().empty());

    RVX::TerrainLODSelection selection;
    lod.SelectLOD(RVX::Vec3(0.0f, 8.0f, 0.0f), nullptr, selection);
    ASSERT_FALSE(selection.nodes.empty());
    EXPECT_EQ(selection.nodes.front().lodMask, RVX::RVX_TERRAIN_LOD_MASK_UNGENERATED);
}

TEST_F(RenderHonestyValidationFixture, TerrainPlaceholderPathsExposeHonestDiagnostics)
{
    const fs::path repoRoot = FindRepoRoot();
    ASSERT_FALSE(repoRoot.empty());

    const std::string heightmapHeader =
        ReadTextFile(repoRoot / "Terrain" / "Private" / "Terrain" / "Heightmap.h");
    const std::string heightmapSource =
        ReadTextFile(repoRoot / "Terrain" / "Private" / "Heightmap.cpp");
    const std::string materialHeader =
        ReadTextFile(repoRoot / "Terrain" / "Private" / "Terrain" / "TerrainMaterial.h");
    const std::string materialSource =
        ReadTextFile(repoRoot / "Terrain" / "Private" / "TerrainMaterial.cpp");
    const std::string lodHeader =
        ReadTextFile(repoRoot / "Terrain" / "Private" / "Terrain" / "TerrainLOD.h");
    const std::string lodSource =
        ReadTextFile(repoRoot / "Terrain" / "Private" / "TerrainLOD.cpp");

    EXPECT_EQ(heightmapSource.find("This is a simplified placeholder"), std::string::npos);
    EXPECT_EQ(heightmapSource.find("Render/GPUUploadService.h"), std::string::npos);
    EXPECT_NE(heightmapHeader.find("IsGPUTextureDataUploaded"), std::string::npos);
    EXPECT_NE(heightmapHeader.find("GetGPUTextureDiagnostic"), std::string::npos);
    EXPECT_NE(heightmapSource.find("Render-owned"), std::string::npos);

    EXPECT_EQ(materialSource.find("This is a simplified placeholder"), std::string::npos);
    EXPECT_NE(materialHeader.find("IsLayerBufferDataUploaded"), std::string::npos);
    EXPECT_EQ(materialHeader.find("RHIBuffer"), std::string::npos);
    EXPECT_EQ(materialSource.find("m_layerBuffer->"), std::string::npos);
    EXPECT_NE(materialSource.find("BuildLayerRenderData"), std::string::npos);

    EXPECT_EQ(lodSource.find("TODO: Calculate neighbor LOD mask"), std::string::npos);
    EXPECT_EQ(lodSource.find("Render/GPUUploadService.h"), std::string::npos);
    EXPECT_NE(lodHeader.find("SupportsCrackPrevention"), std::string::npos);
    EXPECT_NE(lodHeader.find("UsesConservativeHeightBounds"), std::string::npos);
    EXPECT_NE(lodSource.find("Conservative fallback"), std::string::npos);
    EXPECT_EQ(lodSource.find("UploadMappedTerrainBuffer"), std::string::npos);
    EXPECT_NE(lodSource.find("Render-owned"), std::string::npos);
}

TEST_F(RenderHonestyValidationFixture,
       SceneRendererLegacyCollectionAndV4FramePathsAreRemoved)
{
    const fs::path repoRoot = FindRepoRoot();
    ASSERT_FALSE(repoRoot.empty());

    const std::string header =
        ReadTextFile(repoRoot / "Render" / "Include" / "Render" / "Renderer" / "SceneRenderer.h");
    const std::string source =
        ReadTextFile(repoRoot / "Render" / "Private" / "Renderer" / "SceneRenderer.cpp");
    const std::string runtimeSource =
        ReadTextFile(repoRoot / "Render" / "Private" / "Runtime" /
                     "RenderThreadRuntime.cpp");
    const std::string renderContractsCMake =
        ReadTextFile(repoRoot / "RenderContracts" / "CMakeLists.txt");
    const std::string renderFrameValidation =
        ReadTextFile(repoRoot / "RenderContracts" / "Include" /
                     "RenderContracts" / "RenderFrameValidation.h");
    const std::string renderExtractionCMake =
        ReadTextFile(repoRoot / "RenderExtraction" / "CMakeLists.txt");

    EXPECT_EQ(std::string::npos, header.find("SetLegacyCollectionFallbackEnabled"));
    EXPECT_EQ(std::string::npos, header.find("IsLegacyCollectionFallbackEnabled"));
    EXPECT_EQ(std::string::npos, header.find("m_legacyCollectionFallbackEnabled"));
    EXPECT_EQ(std::string::npos, source.find("m_renderScene.CollectFromWorld(world)"));
    EXPECT_EQ(std::string::npos, source.find("SceneRenderCollectionPath::LegacyFallback"));
    EXPECT_EQ(std::string::npos, source.find("RenderFeatureSceneBridge"));
    EXPECT_EQ(std::string::npos, header.find("ApplyFramePacket("));
    EXPECT_EQ(std::string::npos, source.find("ApplyFramePacket("));
    EXPECT_NE(std::string::npos,
              source.find("m_renderScene.ApplyFrameV5(frame, scene, registry)"));
    EXPECT_NE(std::string::npos,
              runtimeSource.find("m_consumer->ConsumeFrameV5("));
    EXPECT_EQ(std::string::npos,
              runtimeSource.find("BuildCompatibilityFrame"));
    EXPECT_EQ(std::string::npos,
              runtimeSource.find("RenderSceneTransportMode"));
    EXPECT_EQ(std::string::npos,
              renderContractsCMake.find("Private/RenderFramePacket.cpp"));
    EXPECT_NE(std::string::npos,
              renderFrameValidation.find(
                  "#include \"RenderContracts/RenderFrameTypes.h\""));
    EXPECT_EQ(std::string::npos,
              renderFrameValidation.find(
                  "#include \"RenderContracts/RenderFramePacket.h\""));
    EXPECT_EQ(std::string::npos,
              renderExtractionCMake.find("Private/RenderFramePacketBuilder.cpp"));
    EXPECT_FALSE(fs::exists(repoRoot / "RenderContracts" / "Include" /
                            "RenderContracts" / "RenderFramePacket.h"));
    EXPECT_FALSE(fs::exists(repoRoot / "RenderContracts" / "Private" /
                            "RenderFramePacket.cpp"));
    EXPECT_FALSE(fs::exists(repoRoot / "RenderExtraction" / "Include" /
                            "RenderExtraction" / "RenderFramePacketBuilder.h"));
    EXPECT_FALSE(fs::exists(repoRoot / "RenderExtraction" / "Private" /
                            "RenderFramePacketBuilder.cpp"));
    EXPECT_NE(std::string::npos, source.find("m_viewData.SetupFromSnapshot("));
}

TEST_F(RenderHonestyValidationFixture, ColorGradingRequiresResourcesBeforeSupported)
{
    RVX::PostProcessSettings settings;
    settings.enableColorGrading = true;

    RVX::ColorGradingPass colorGrading;
    colorGrading.Configure(settings);

    EXPECT_TRUE(colorGrading.IsRequestedEnabled());
    EXPECT_EQ(colorGrading.GetMode(), RVX::ColorGradingMode::LDR);
    EXPECT_FALSE(colorGrading.IsSupported());
    EXPECT_FALSE(colorGrading.IsEnabled());
    EXPECT_FALSE(colorGrading.GetUnsupportedReason().empty());
}

TEST_F(RenderHonestyValidationFixture, ChromaticAberrationRequiresResourcesBeforeSupported)
{
    RVX::PostProcessSettings settings;
    settings.enableChromaticAberration = true;

    RVX::ChromaticAberrationPass chromaticAberration;
    chromaticAberration.Configure(settings);

    EXPECT_TRUE(chromaticAberration.IsRequestedEnabled());
    EXPECT_FALSE(chromaticAberration.IsSupported());
    EXPECT_FALSE(chromaticAberration.IsEnabled());
    EXPECT_FALSE(chromaticAberration.GetUnsupportedReason().empty());
}

TEST_F(RenderHonestyValidationFixture, ChromaticAberrationRejectsSpectralRequests)
{
    RVX::PostProcessSettings settings;
    settings.enableChromaticAberration = true;

    RVX::ChromaticAberrationPass chromaticAberration;
    chromaticAberration.Configure(settings);
    chromaticAberration.SetSpectralSampling(true);

    EXPECT_TRUE(chromaticAberration.IsRequestedEnabled());
    EXPECT_TRUE(chromaticAberration.IsSpectralSampling());
    EXPECT_FALSE(chromaticAberration.IsSupported());
    EXPECT_FALSE(chromaticAberration.IsEnabled());
    EXPECT_NE(chromaticAberration.GetUnsupportedReason().find("spectral"), std::string::npos);
}

TEST_F(RenderHonestyValidationFixture, ColorGradingRejectsHDRAndLUTRequests)
{
    RVX::PostProcessSettings settings;
    settings.enableColorGrading = true;

    RVX::ColorGradingPass hdrMode;
    hdrMode.Configure(settings);
    hdrMode.SetMode(RVX::ColorGradingMode::HDR);
    EXPECT_TRUE(hdrMode.IsRequestedEnabled());
    EXPECT_FALSE(hdrMode.IsSupported());
    EXPECT_FALSE(hdrMode.IsEnabled());
    EXPECT_NE(hdrMode.GetUnsupportedReason().find("HDR"), std::string::npos);

    RVX::ColorGradingPass lutMode;
    lutMode.Configure(settings);
    lutMode.SetUseLUT(true);
    EXPECT_TRUE(lutMode.IsRequestedEnabled());
    EXPECT_FALSE(lutMode.IsSupported());
    EXPECT_FALSE(lutMode.IsEnabled());
    EXPECT_NE(lutMode.GetUnsupportedReason().find("LUT"), std::string::npos);
    EXPECT_EQ(lutMode.BakeToLUT(nullptr), nullptr);
}

TEST_F(RenderHonestyValidationFixture, FXAARequiresResourcesBeforeSupported)
{
    RVX::PostProcessSettings settings;
    settings.enableFXAA = true;

    RVX::FXAAPass fxaa;
    fxaa.Configure(settings);

    EXPECT_TRUE(fxaa.IsRequestedEnabled());
    EXPECT_FALSE(fxaa.IsSupported());
    EXPECT_FALSE(fxaa.IsEnabled());
    EXPECT_FALSE(fxaa.GetUnsupportedReason().empty());
}

TEST_F(RenderHonestyValidationFixture, VignetteRequiresResourcesBeforeSupported)
{
    RVX::PostProcessSettings settings;
    settings.enableVignette = true;

    RVX::VignettePass vignette;
    vignette.Configure(settings);

    EXPECT_TRUE(vignette.IsRequestedEnabled());
    EXPECT_FALSE(vignette.IsSupported());
    EXPECT_FALSE(vignette.IsEnabled());
    EXPECT_FALSE(vignette.GetUnsupportedReason().empty());
}

TEST_F(RenderHonestyValidationFixture, BloomRequiresResourcesBeforeSupported)
{
    RVX::PostProcessSettings settings;
    settings.enableBloom = true;

    RVX::BloomPass bloom;
    bloom.Configure(settings);

    EXPECT_TRUE(bloom.IsRequestedEnabled());
    EXPECT_FALSE(bloom.IsSupported());
    EXPECT_FALSE(bloom.IsEnabled());
    EXPECT_FALSE(bloom.GetUnsupportedReason().empty());
}

TEST_F(RenderHonestyValidationFixture, ToneMappingRequiresResourcesBeforeSupported)
{
    RVX::PostProcessSettings settings;
    settings.enableToneMapping = true;

    RVX::ToneMappingPass toneMapping;
    toneMapping.Configure(settings);

    EXPECT_TRUE(toneMapping.IsRequestedEnabled());
    EXPECT_FALSE(toneMapping.IsSupported());
    EXPECT_FALSE(toneMapping.IsEnabled());
    EXPECT_FALSE(toneMapping.GetUnsupportedReason().empty());
}

TEST_F(RenderHonestyValidationFixture, StandalonePostProcessFeaturesExposeUnsupportedState)
{
    RVX::TAA taa;
    RVX::SSAO ssao;
    RVX::SSR ssr;

    EXPECT_FALSE(taa.IsSupported());
    EXPECT_FALSE(taa.IsEnabled());
    EXPECT_FALSE(taa.GetUnsupportedReason().empty());

    EXPECT_FALSE(ssao.IsSupported());
    EXPECT_FALSE(ssao.IsEnabled());
    EXPECT_FALSE(ssao.GetUnsupportedReason().empty());

    EXPECT_FALSE(ssr.IsSupported());
    EXPECT_FALSE(ssr.IsEnabled());
    EXPECT_FALSE(ssr.GetUnsupportedReason().empty());
}

TEST_F(RenderHonestyValidationFixture, SkyAndAtmosphereDoNotReportReadyWithoutPipelines)
{
    RVX::SkyboxPass skybox;
    EXPECT_TRUE(skybox.IsRequestedEnabled());
    EXPECT_FALSE(skybox.IsDrawReady());
    EXPECT_FALSE(skybox.IsEnabled());
    EXPECT_FALSE(skybox.GetUnsupportedReason().empty());

    RVX::AtmosphericScattering atmosphere;
    EXPECT_TRUE(atmosphere.IsRequestedEnabled());
    EXPECT_FALSE(atmosphere.IsSupported());
    EXPECT_FALSE(atmosphere.IsEnabled());
    EXPECT_FALSE(atmosphere.GetUnsupportedReason().empty());
    EXPECT_TRUE(atmosphere.IsCpuAnalyticBaselineAvailable());

    const RVX::AtmosphericScatteringDiagnostics diagnostics = atmosphere.GetDiagnostics();
    EXPECT_TRUE(diagnostics.requested);
    EXPECT_FALSE(diagnostics.initialized);
    EXPECT_FALSE(diagnostics.gpuLutSupported);
    EXPECT_TRUE(diagnostics.cpuAnalyticBaselineAvailable);
    EXPECT_EQ(diagnostics.implementationTier,
              RVX::AtmosphericScatteringImplementationTier::CpuAnalyticBaseline);
    EXPECT_STREQ(RVX::GetAtmosphericScatteringImplementationTierName(diagnostics.implementationTier),
                 "CpuAnalyticBaseline");
    EXPECT_NE(diagnostics.unsupportedReason.find("CPU analytic baseline"), std::string::npos);

    const RVX::Vec3 zenith = atmosphere.GetSkyColor(RVX::Vec3(0.0f, 1.0f, 0.0f));
    const RVX::Vec3 horizon = atmosphere.GetSkyColor(RVX::Vec3(1.0f, 0.02f, 0.0f));
    EXPECT_GT(zenith.z, zenith.x);
    EXPECT_GT(horizon.z, horizon.x);
    EXPECT_GT(std::abs(zenith.z - horizon.z), 0.01f);

    const RVX::AtmosphericScatteringConfig& config = atmosphere.GetConfig();
    const RVX::Vec3 transmittance =
        atmosphere.GetTransmittance(RVX::Vec3(0.0f, config.planetRadius + 1000.0f, 0.0f),
                                    RVX::Vec3(1.0f, 0.0f, 0.0f),
                                    100000.0f);
    EXPECT_LT(transmittance.x, 1.0f);
    EXPECT_LT(transmittance.y, 1.0f);
    EXPECT_LT(transmittance.z, 1.0f);
}

TEST_F(RenderHonestyValidationFixture, ParticleRenderingAndSimulationExposeDisconnectedState)
{
    auto system = RVX::Particle::ParticleSystem::Create("honesty-particles");
    RVX::Particle::ParticleSystemInstance instance(system);
    instance.Play();
    instance.Simulate(0.016f);

    EXPECT_FALSE(instance.IsSimulationSupported());
    EXPECT_FALSE(instance.GetSimulationUnsupportedReason().empty());
    EXPECT_EQ(instance.GetSimulationTime(), 0.0f);

    RVX::ParticleRenderSnapshot snapshot;
    snapshot.BeginBuild(1);
    RVX::ParticleRenderSnapshotItem item;
    item.instanceId = 1;
    item.systemId = 1;
    item.systemName = "honesty-particles";
    item.payloadStatus = RVX::ParticleRenderSnapshotPayloadStatus::MetadataOnly;
    item.aliveParticleCount = 4;
    item.renderPayloadAvailable = false;
    snapshot.items.push_back(item);
    snapshot.metadata.totalAliveParticles = item.aliveParticleCount;
    snapshot.MarkComplete();

    RVX::ParticleFeaturePass pass;
    pass.SetSnapshot(&snapshot);
    RVX::RenderPassStatus status = pass.GetStatus();
    EXPECT_TRUE(status.requestedEnabled);
    EXPECT_FALSE(status.supported);
    EXPECT_FALSE(status.enabled);
    EXPECT_FALSE(status.unsupportedReason.empty());
    EXPECT_NE(status.unsupportedReason.find("metadata-only"), std::string::npos);
    EXPECT_FALSE(pass.IsEnabled());
}

TEST_F(RenderHonestyValidationFixture, RenderGraphCompileDiagnosticsExposeInvalidHandles)
{
    NullDevice device;
    RVX::RenderGraph graph;
    RVX::RenderGraphValidationAccess::SetDevice(graph, &device);

    RVX::RHITextureDesc outputDesc = RVX::RHITextureDesc::RenderTarget(16, 16, RVX::RHIFormat::RGBA8_UNORM);
    outputDesc.debugName = "DiagnosticOutput";
    RVX::RGTextureHandle output = graph.CreateTexture(outputDesc);
    graph.SetExportState(output, RVX::RHIResourceState::ShaderResource);

    struct PassData
    {
        RVX::RGTextureHandle invalidInput;
        RVX::RGTextureHandle output;
    };

    graph.AddPass<PassData>(
        "InvalidDiagnosticPass",
        RVX::RenderGraphPassType::Graphics,
        [output](RVX::RenderGraphBuilder& builder, PassData& data)
        {
            data.invalidInput = RVX::RGTextureHandle{99};
            data.output = output;
            builder.Read(data.invalidInput);
            builder.Write(data.output);
        },
        [](const PassData&, RVX::RHICommandContext&) {});

    RVX::RenderGraphValidationAccess::Compile(graph);

    const RVX::RenderGraph::CompileStats& stats = graph.GetCompileStats();
    EXPECT_FALSE(stats.compileValid);
    EXPECT_EQ(stats.invalidResourceUsageCount, 1u);
    EXPECT_EQ(stats.validationErrorCount, 1u);

    const auto& diagnostics = graph.GetCompileDiagnostics();
    ASSERT_FALSE(diagnostics.empty());
    const std::string invalidHandleText = std::string("invalid texture handle ") +
                                         std::to_string(RVX::RVX_INVALID_INDEX);
    EXPECT_NE(std::find_if(diagnostics.begin(),
                           diagnostics.end(),
                           [invalidHandleText](const std::string& diagnostic)
                           {
                               return diagnostic.find("InvalidDiagnosticPass") != std::string::npos &&
                                      diagnostic.find(invalidHandleText) != std::string::npos;
                           }),
              diagnostics.end());
}
TEST_F(RenderHonestyValidationFixture, RenderGraphCompileDiagnosticsExposeReadBeforeWrite)
{
    NullDevice device;
    RVX::RenderGraph graph;
    RVX::RenderGraphValidationAccess::SetDevice(graph, &device);

    RVX::RHITextureDesc inputDesc = RVX::RHITextureDesc::RenderTarget(16, 16, RVX::RHIFormat::RGBA8_UNORM);
    inputDesc.debugName = "UninitializedInput";
    RVX::RGTextureHandle input = graph.CreateTexture(inputDesc);

    RVX::RHITextureDesc outputDesc = RVX::RHITextureDesc::RenderTarget(16, 16, RVX::RHIFormat::RGBA8_UNORM);
    outputDesc.debugName = "ProducedOutput";
    RVX::RGTextureHandle output = graph.CreateTexture(outputDesc);
    graph.SetExportState(output, RVX::RHIResourceState::ShaderResource);

    struct PassData
    {
        RVX::RGTextureHandle input;
        RVX::RGTextureHandle output;
    };

    graph.AddPass<PassData>(
        "ReadBeforeWritePass",
        RVX::RenderGraphPassType::Graphics,
        [input, output](RVX::RenderGraphBuilder& builder, PassData& data)
        {
            data.input = input;
            data.output = output;
            builder.Read(data.input);
            builder.Write(data.output);
        },
        [](const PassData&, RVX::RHICommandContext&) {});

    RVX::RenderGraphValidationAccess::Compile(graph);

    const RVX::RenderGraph::CompileStats& stats = graph.GetCompileStats();
    EXPECT_FALSE(stats.compileValid);
    EXPECT_EQ(stats.readBeforeWriteHazardCount, 1u);
    EXPECT_EQ(stats.validationErrorCount, 1u);

    const auto& diagnostics = graph.GetCompileDiagnostics();
    ASSERT_FALSE(diagnostics.empty());
    EXPECT_NE(std::find_if(diagnostics.begin(),
                           diagnostics.end(),
                           [](const std::string& diagnostic)
                           {
                               return diagnostic.find("ReadBeforeWritePass") != std::string::npos &&
                                      diagnostic.find("reads transient texture") != std::string::npos &&
                                      diagnostic.find("before any producing write") != std::string::npos;
                           }),
              diagnostics.end());
}
