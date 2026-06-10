#include "Core/Log.h"
#include "Core/Serialization/Serialization.h"
#include "Render/Debug/GPUProfiler.h"
#include "Render/Material/MaterialBinder.h"
#include "Render/Material/MaterialTemplate.h"
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
#include "Resource/Loader/TextureLoader.h"
#include "RHI/RHICommandContext.h"
#include "RHI/RHI.h"
#include "Particle/ParticleSystem.h"
#include "Particle/ParticleSystemInstance.h"
#include "Particle/Rendering/ParticlePass.h"
#include "Particle/Rendering/ParticleRenderer.h"
#include "Tools/AssetDatabase.h"
#include "Tools/AssetPipeline.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <utility>
#include <vector>

namespace
{
    namespace fs = std::filesystem;

    class NullDevice final : public RVX::IRHIDevice
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

    fs::path MakeTempDir(const char* name)
    {
        const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
        fs::path dir = fs::temp_directory_path() / (std::string(name) + "_" + std::to_string(ticks));
        fs::create_directories(dir);
        return dir;
    }

    void WriteTextFile(const fs::path& path, const std::string& text)
    {
        fs::create_directories(path.parent_path());
        std::ofstream file(path, std::ios::binary);
        file << text;
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

    manager.Unload(texturePath.string());
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
    settings.enableChromaticAberration = true;
    settings.enableFilmGrain = true;
    settings.enableVolumetricLighting = true;

    RVX::ColorGradingPass colorGrading;
    RVX::DOFPass dof;
    RVX::MotionBlurPass motionBlur;
    RVX::ChromaticAberrationPass chromaticAberration;
    RVX::FilmGrainPass filmGrain;
    RVX::VolumetricLightingPass volumetricLighting;

    RVX::IPostProcessPass* passes[] = {
        &colorGrading,
        &dof,
        &motionBlur,
        &chromaticAberration,
        &filmGrain,
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

    RVX::Particle::ParticleRenderer renderer;
    EXPECT_FALSE(renderer.IsRenderingSupported());
    EXPECT_FALSE(renderer.GetUnsupportedReason().empty());

    RVX::Particle::ParticlePass pass;
    RVX::RenderPassStatus status = pass.GetStatus();
    EXPECT_FALSE(status.supported);
    EXPECT_FALSE(status.enabled);
    EXPECT_FALSE(status.unsupportedReason.empty());

    pass.SetRenderer(&renderer);
    status = pass.GetStatus();
    EXPECT_TRUE(status.requestedEnabled);
    EXPECT_FALSE(status.supported);
    EXPECT_FALSE(status.enabled);
    EXPECT_FALSE(status.unsupportedReason.empty());
    EXPECT_FALSE(pass.IsEnabled());
}

TEST_F(RenderHonestyValidationFixture, ParticleRendererDrawsReturnFalseWhenUnsupported)
{
    NullDevice device;
    NoOpCommandContext ctx;
    RVX::ViewData view;
    auto system = RVX::Particle::ParticleSystem::Create("draw-return-particles");
    RVX::Particle::ParticleSystemInstance instance(system);
    instance.Play();

    RVX::Particle::ParticleRenderer renderer;
    EXPECT_FALSE(renderer.DrawParticles(ctx, nullptr, view, nullptr));
    EXPECT_FALSE(renderer.DrawParticlesIndirect(ctx, nullptr, view, nullptr));

    renderer.Initialize(&device);
    EXPECT_TRUE(renderer.IsInitialized());
    EXPECT_FALSE(renderer.IsRenderingSupported());
    EXPECT_FALSE(renderer.GetUnsupportedReason().empty());

    EXPECT_FALSE(renderer.DrawParticles(ctx, &instance, view, nullptr));
    EXPECT_FALSE(renderer.DrawParticlesIndirect(ctx, &instance, view, nullptr));
    EXPECT_EQ(ctx.drawIndexedCount, 0u);
    EXPECT_EQ(ctx.drawIndexedIndirectCount, 0u);
}

TEST_F(RenderHonestyValidationFixture, ParticlePassUnsupportedSetupDeclaresNoGraphResources)
{
    NullDevice device;
    RVX::RenderGraph graph;
    graph.SetDevice(&device);

    RVX::ViewData view;
    RVX::RHITextureDesc colorDesc = RVX::RHITextureDesc::RenderTarget(32, 32, RVX::RHIFormat::RGBA8_UNORM);
    colorDesc.debugName = "ParticleUnsupportedColor";
    view.colorTarget = graph.CreateTexture(colorDesc);

    RVX::RHITextureDesc depthDesc = RVX::RHITextureDesc::DepthStencil(32, 32, RVX::RHIFormat::D32_FLOAT);
    depthDesc.debugName = "ParticleUnsupportedDepth";
    view.depthTarget = graph.CreateTexture(depthDesc);

    RVX::Particle::ParticleRenderer renderer;
    RVX::Particle::ParticlePass pass;
    pass.SetRenderer(&renderer);
    pass.AddToGraph(graph, view);
    graph.Compile();

    const RVX::RenderGraph::CompileStats& stats = graph.GetCompileStats();
    EXPECT_TRUE(stats.compileValid);
    EXPECT_EQ(stats.totalPasses, 1u);
    EXPECT_EQ(stats.emptyPassUsageCount, 1u);
    EXPECT_EQ(stats.culledPasses, 1u);

    NoOpCommandContext ctx;
    graph.Execute(ctx);
    EXPECT_EQ(ctx.drawIndexedCount, 0u);
    EXPECT_EQ(ctx.drawIndexedIndirectCount, 0u);
}
