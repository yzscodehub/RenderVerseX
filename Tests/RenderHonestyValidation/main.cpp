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
#include "Render/Sky/AtmosphericScattering.h"
#include "Resource/Loader/TextureLoader.h"
#include "RHI/RHI.h"
#include "Particle/ParticleSystem.h"
#include "Particle/ParticleSystemInstance.h"
#include "Particle/Rendering/ParticlePass.h"
#include "Particle/Rendering/ParticleRenderer.h"
#include "Tools/AssetDatabase.h"
#include "Tools/AssetPipeline.h"

#include <gtest/gtest.h>

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
}

TEST_F(RenderHonestyValidationFixture, TextureReferenceFallbackIsObservable)
{
    RVX::Resource::TextureLoader loader(nullptr);
    RVX::Resource::TextureReference invalidReference;

    RVX::Resource::TextureResource* texture = loader.LoadFromReference(invalidReference, "model.gltf");

    ASSERT_NE(texture, nullptr);
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
    EXPECT_EQ(loader.GetLastLoadStatus(), RVX::Resource::TextureLoadStatus::Loaded);

    RVX::Resource::TextureReference invalidReference;
    ASSERT_NE(loader.LoadFromReference(invalidReference, "model.gltf"), nullptr);
    ASSERT_TRUE(loader.WasLastLoadFallback());

    RVX::Resource::TextureResource* cachedMemory = loader.LoadFromMemory(
        rgba.data(), rgba.size(), cachePath.string(), RVX::Resource::TextureUsage::Color, true, 1, 1);
    EXPECT_EQ(cachedMemory, first);
    EXPECT_EQ(loader.GetLastLoadStatus(), RVX::Resource::TextureLoadStatus::Loaded);
    EXPECT_FALSE(loader.WasLastLoadFallback());
    EXPECT_TRUE(loader.GetLastLoadError().empty());

    ASSERT_NE(loader.LoadFromReference(invalidReference, "model.gltf"), nullptr);
    ASSERT_TRUE(loader.WasLastLoadFallback());

    RVX::Resource::TextureResource* cachedFile = loader.LoadFromFile(cachePath.string());
    EXPECT_EQ(cachedFile, first);
    EXPECT_EQ(loader.GetLastLoadStatus(), RVX::Resource::TextureLoadStatus::Loaded);
    EXPECT_FALSE(loader.WasLastLoadFallback());
    EXPECT_TRUE(loader.GetLastLoadError().empty());

    std::vector<RVX::uint8> embeddedPixels = { 128, 128, 255, 255 };
    RVX::Resource::TextureReference embedded = RVX::Resource::TextureReference::CreateEmbeddedRaw(
        std::move(embeddedPixels), 7, 1, 1, RVX::Resource::TextureUsage::Normal, false);

    RVX::Resource::TextureResource* embeddedFirst = loader.LoadFromReference(embedded, "model.gltf");
    ASSERT_NE(embeddedFirst, nullptr);
    EXPECT_EQ(loader.GetLastLoadStatus(), RVX::Resource::TextureLoadStatus::Loaded);

    ASSERT_NE(loader.LoadFromReference(invalidReference, "model.gltf"), nullptr);
    ASSERT_TRUE(loader.WasLastLoadFallback());

    RVX::Resource::TextureResource* embeddedCached = loader.LoadFromReference(embedded, "model.gltf");
    EXPECT_EQ(embeddedCached, embeddedFirst);
    EXPECT_EQ(loader.GetLastLoadStatus(), RVX::Resource::TextureLoadStatus::Loaded);
    EXPECT_FALSE(loader.WasLastLoadFallback());
    EXPECT_TRUE(loader.GetLastLoadError().empty());

    manager.Shutdown();
    fs::remove_all(cachePath.parent_path());
}

TEST_F(RenderHonestyValidationFixture, PostProcessStubPassesAreUnsupportedAndDisabled)
{
    RVX::PostProcessSettings settings;
    settings.enableDOF = true;
    settings.enableMotionBlur = true;
    settings.enableVignette = true;
    settings.enableChromaticAberration = true;
    settings.enableFilmGrain = true;
    settings.enableVolumetricLighting = true;

    RVX::ToneMappingPass toneMapping;
    RVX::BloomPass bloom;
    RVX::FXAAPass fxaa;
    RVX::ColorGradingPass colorGrading;
    RVX::DOFPass dof;
    RVX::MotionBlurPass motionBlur;
    RVX::VignettePass vignette;
    RVX::ChromaticAberrationPass chromaticAberration;
    RVX::FilmGrainPass filmGrain;
    RVX::VolumetricLightingPass volumetricLighting;

    RVX::IPostProcessPass* passes[] = {
        &toneMapping,
        &bloom,
        &fxaa,
        &colorGrading,
        &dof,
        &motionBlur,
        &vignette,
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
    EXPECT_FALSE(pass.IsEnabled());
}
