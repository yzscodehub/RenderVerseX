#include "Core/Log.h"
#include "Particle/Emitters/PointEmitter.h"
#include "Particle/ParticleSystemLoader.h"
#include "Resource/Cooked/CookedParticleEffectArtifact.h"
#include "Resource/CookManifest.h"
#include "Resource/ResourceManager.h"
#include "Resource/RuntimeResourcePolicy.h"
#include "Resource/Types/TextureResource.h"
#include "Tools/Importers/ParticleEffectImporter.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

namespace
{
    namespace fs = std::filesystem;

    class ScopedTemporaryDirectory final
    {
    public:
        ScopedTemporaryDirectory()
        {
            const auto token = std::chrono::steady_clock::now()
                .time_since_epoch().count();
            m_path = fs::temp_directory_path() /
                ("RVX_ParticleEffectCook_" + std::to_string(token));
            std::error_code error;
            fs::create_directories(m_path, error);
            EXPECT_FALSE(error) << error.message();
        }

        ~ScopedTemporaryDirectory()
        {
            std::error_code error;
            fs::remove_all(m_path, error);
        }

        const fs::path& Get() const noexcept { return m_path; }

    private:
        fs::path m_path;
    };

    bool WriteText(const fs::path& path, const std::string& text)
    {
        std::error_code error;
        fs::create_directories(path.parent_path(), error);
        if (error) return false;
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output.write(text.data(), static_cast<std::streamsize>(text.size()));
        return output.good();
    }

    bool WriteRgbaTga(const fs::path& path)
    {
        std::error_code error;
        fs::create_directories(path.parent_path(), error);
        if (error) return false;
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        if (!output) return false;

        // Uncompressed 4x4 BGRA TGA with a top-left origin and alpha. This
        // exactly accommodates the 4x4 flipbook grid authored below.
        const RVX::uint8 header[18] = {
            0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0,
            4, 0, 4, 0, 32, 0x28};
        std::vector<RVX::uint8> pixels(4u * 4u * 4u);
        for (size_t index = 0; index < pixels.size(); index += 4u)
        {
            pixels[index + 0u] = 10u;
            pixels[index + 1u] = 100u;
            pixels[index + 2u] = 255u;
            pixels[index + 3u] = 255u;
        }
        output.write(reinterpret_cast<const char*>(header), sizeof(header));
        output.write(reinterpret_cast<const char*>(pixels.data()),
                     static_cast<std::streamsize>(pixels.size()));
        return output.good();
    }

    bool WriteCookedRva(const fs::path& path,
                        RVX::uint32 width,
                        RVX::uint32 height,
                        const char* usage)
    {
        std::error_code error;
        fs::create_directories(path.parent_path(), error);
        if (error || usage == nullptr || width == 0u || height == 0u)
        {
            return false;
        }
        const size_t byteCount = static_cast<size_t>(width) * height * 4u;
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        if (!output)
        {
            return false;
        }
        output << "RVX_TEXTURE_PREBAKE_V1\n"
               << "width=" << width << "\n"
               << "height=" << height << "\n"
               << "depth=1\n"
               << "mipLevels=1\n"
               << "arrayLayers=1\n"
               << "format=RGBA8\n"
               << "usage=" << usage << "\n"
               << "srgb=1\n"
               << "compression=UncompressedRGBA\n"
               << "dataSize=" << byteCount << "\n"
               << "RVX_TEXTURE_DATA_BEGIN\n";
        const std::vector<RVX::uint8> pixels(byteCount, 0xffu);
        output.write(reinterpret_cast<const char*>(pixels.data()),
                     static_cast<std::streamsize>(pixels.size()));
        output << "\nRVX_TEXTURE_PREBAKE_END\n";
        return output.good();
    }

    std::vector<RVX::uint8> ReadBytes(const fs::path& path)
    {
        std::ifstream input(path, std::ios::binary);
        return {std::istreambuf_iterator<char>(input),
                std::istreambuf_iterator<char>()};
    }

    bool WriteBytes(const fs::path& path, const std::vector<RVX::uint8>& bytes)
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        if (!output) return false;
        output.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
        return output.good();
    }

    std::map<std::string, std::vector<RVX::uint8>> SnapshotTree(const fs::path& root)
    {
        std::map<std::string, std::vector<RVX::uint8>> snapshot;
        for (const fs::directory_entry& entry : fs::recursive_directory_iterator(root))
        {
            if (entry.is_regular_file())
            {
                snapshot.emplace(entry.path().lexically_relative(root).generic_string(),
                                 ReadBytes(entry.path()));
            }
        }
        return snapshot;
    }

    std::string Quote(const fs::path& path)
    {
        std::string quoted = "\"";
        for (const char character : path.string())
        {
            if (character == '\"') quoted += '\\';
            quoted += character;
        }
        return quoted + "\"";
    }

    std::string WrapSystemCommand(std::string command)
    {
#if defined(_WIN32)
        return "\"" + command + "\"";
#else
        return command;
#endif
    }

#if defined(RVX_COOK_EXECUTABLE_PATH)
    int RunCook(const fs::path& sourceRoot, const fs::path& outputRoot)
    {
        const fs::path manifestPath = outputRoot / "CookManifest.rvxmanifest";
        const std::string command = WrapSystemCommand(
            Quote(fs::path(RVX_COOK_EXECUTABLE_PATH)) +
            " --source " + Quote(sourceRoot) +
            " --output " + Quote(outputRoot) +
            " --manifest " + Quote(manifestPath) +
            " --texture-compression none --fail-on-errors");
        return std::system(command.c_str());
    }
#endif

    std::string ValidAuthoringJson()
    {
        return R"json({
  "schema":"RVX_PARTICLE_EFFECT_AUTHORING_V1",
  "logicalAssetId":"Effects/Fire",
  "render":{"simulation":"CPU","renderMode":"Billboard","blendMode":"Additive"},
  "emitter":{"maxParticles":128,"looping":true,"durationSeconds":3.0,"emissionRate":32.0,"position":[0,0,0],"speed":[0.5,1.5],"lifetime":[0.5,1.0],"initialColor":[4,1,0.1,1],"size":[0.1,0.3],"rotationDegrees":[0,0],"rotationSpeedDegrees":[0,0]},
  "flipbook":{"textureArtifactPath":"99_Fire.rva","tiles":[4,4],"firstFrame":0,"frameCount":16,"framesPerSecond":24.0,"uvOffset":[0,0],"uvScale":[1,1]},
  "softDepth":{"policy":"Required","fadeDistanceMeters":0.25,"contrastPower":1.0},
  "distortion":{"distortionStrengthPixels":2.0},
  "reactiveMask":{"reactiveContribution":0.8},
  "lightHint":{"enabled":true,"color":[1,0.5,0.1],"intensityCandela":40.0,"rangeMeters":2.0,"flickerFrequencyHz":2.0}
})json";
    }

    class ParticleEffectCookPipelineValidation : public ::testing::Test
    {
    protected:
        static void SetUpTestSuite() { RVX::Log::Initialize(); }
        static void TearDownTestSuite() { RVX::Log::Shutdown(); }
    };
}

TEST_F(ParticleEffectCookPipelineValidation,
     RVXCookProducesDeterministicEffectManifestAndPreparesContainedAtlasDependency)
{
#ifndef RVX_COOK_EXECUTABLE_PATH
    GTEST_SKIP() << "RVXCook executable path was not provided by CMake";
#else
    ScopedTemporaryDirectory fixture;
    const fs::path source = fixture.Get() / "Source";
    const fs::path firstOutput = fixture.Get() / "CookedA";
    const fs::path secondOutput = fixture.Get() / "CookedB";
    // The effect sorts before its atlas. CookDirectory's explicit Effect
    // phase must nevertheless cook the atlas first.
    ASSERT_TRUE(WriteRgbaTga(source / "Vfx" / "99_Fire.tga"));
    ASSERT_TRUE(WriteText(source / "Vfx" / "00_Fire.rvxparticle", ValidAuthoringJson()));

    // The authored effect references only the canonical cooked dependency path;
    // runtime never consumes source image data.
    ASSERT_EQ(RunCook(source, firstOutput), 0);
    ASSERT_EQ(RunCook(source, secondOutput), 0);
    EXPECT_EQ(SnapshotTree(firstOutput), SnapshotTree(secondOutput));

    const fs::path effectPath = firstOutput / "Vfx" / "00_Fire.rvxvfx";
    const fs::path atlasPath = firstOutput / "Vfx" / "99_Fire.rva";
    const fs::path manifestPath = firstOutput / "CookManifest.rvxmanifest";
    ASSERT_TRUE(fs::is_regular_file(effectPath));
    ASSERT_TRUE(fs::is_regular_file(atlasPath));
    ASSERT_TRUE(fs::is_regular_file(manifestPath));

    RVX::Resource::CookManifest manifest;
    std::string error;
    ASSERT_TRUE(RVX::Resource::LoadCookManifest(manifestPath, manifest, error)) << error;
    ASSERT_EQ(manifest.entries.size(), 2u);
    const auto effectEntry = std::find_if(
        manifest.entries.begin(), manifest.entries.end(),
        [](const RVX::Resource::CookManifestEntry& entry)
        {
            return entry.type == RVX::Resource::CookAssetType::Effect;
        });
    ASSERT_NE(effectEntry, manifest.entries.end());
    EXPECT_TRUE(effectEntry->success);
    EXPECT_EQ(effectEntry->sourcePath, "Vfx/00_Fire.rvxparticle");
    EXPECT_EQ(effectEntry->outputPath, "Vfx/00_Fire.rvxvfx");
    EXPECT_FALSE(effectEntry->artifacts.empty());
    EXPECT_TRUE(std::any_of(effectEntry->artifacts.begin(), effectEntry->artifacts.end(),
        [](const RVX::Resource::CookFileIdentity& artifact)
        {
            return artifact.relativePath == "Vfx/00_Fire.rvxvfx";
        }));
    EXPECT_NE(std::find_if(manifest.entries.begin(), manifest.entries.end(),
        [](const RVX::Resource::CookManifestEntry& entry)
        {
            return entry.type == RVX::Resource::CookAssetType::Texture && entry.success &&
                   entry.outputPath == "Vfx/99_Fire.rva";
        }), manifest.entries.end());

    RVX::Resource::CookedParticleEffectArtifact artifact;
    const std::vector<RVX::uint8> bytes = ReadBytes(effectPath);
    ASSERT_TRUE(RVX::Resource::DeserializeCookedParticleEffectArtifact(
        bytes, artifact, error)) << error;
    EXPECT_EQ(artifact.logicalAssetId, "Effects/Fire");
    EXPECT_NE(artifact.flipbook.expectedContentHash, RVX::Hash::SHA256Digest{});
    ASSERT_EQ(effectEntry->dependencies.size(), 1u);
    EXPECT_EQ(effectEntry->dependencies[0].relativePath, "Vfx/99_Fire.rva");
    EXPECT_EQ(effectEntry->dependencies[0].sha256,
              RVX::Hash::FormatSHA256Digest(artifact.flipbook.expectedContentHash));

    RVX::Particle::ParticleSystemLoader loader;
    EXPECT_FALSE(loader.Supports({RVX::Particle::ResourceType_ParticleSystem,
                                  RVX::Resource::ResourceLoadDomain::Source,
                                  effectPath.string()}));
    EXPECT_TRUE(loader.Supports({RVX::Particle::ResourceType_ParticleSystem,
                                 RVX::Resource::ResourceLoadDomain::Cooked,
                                 effectPath.string()}));

    RVX::Resource::ResourceLoadPreparationContext context;
    context.requestedPath = effectPath.string();
    context.resolvedPath = effectPath.string();
    context.assetKey = RVX::Resource::MakeAssetKey(
        context.requestedPath, RVX::Particle::ResourceType_ParticleSystem);
    context.resourceIdentityPath = context.assetKey.canonicalPath;
    context.rootResourceId = RVX::Resource::GenerateResourceId(context.resourceIdentityPath);
    RVX::Resource::PreparedResourceBundle bundle;
    RVX::Resource::ResourceLoadError loadError;
    ASSERT_TRUE(loader.Prepare(context, bundle, loadError)) << loadError.message;
    ASSERT_EQ(bundle.GetEntries().size(), 2u);
    auto* root = dynamic_cast<RVX::Particle::ParticleSystemResource*>(
        bundle.GetRoot().Get());
    ASSERT_NE(root, nullptr);
    const RVX::Resource::ResourceId atlasResourceId =
        root->GetAtlasTextureResourceId();
    EXPECT_NE(atlasResourceId, RVX::Resource::InvalidResourceId);
    const auto dependency = std::find_if(
        bundle.GetEntries().begin(), bundle.GetEntries().end(),
        [](const RVX::Resource::PreparedResourceEntry& entry)
        {
            return !entry.isRoot;
        });
    ASSERT_NE(dependency, bundle.GetEntries().end());
    ASSERT_NE(dynamic_cast<RVX::Resource::TextureResource*>(
        dependency->resource.Get()), nullptr);
    EXPECT_EQ(dependency->resource.GetId(), atlasResourceId);
    ASSERT_TRUE(root->GetSystem());
    ASSERT_EQ(root->GetSystem()->emitters.size(), 1u);
    const auto* emitter = dynamic_cast<const RVX::Particle::PointEmitter*>(
        root->GetSystem()->GetEmitter(0u));
    ASSERT_NE(emitter, nullptr);
    EXPECT_EQ(emitter->initialVelocityDirection.min, RVX::Vec3(0.0f, 1.0f, 0.0f));
    EXPECT_EQ(emitter->initialVelocityDirection.max, RVX::Vec3(0.0f, 1.0f, 0.0f));
    EXPECT_EQ(emitter->initialColor.min, RVX::Vec4(4.0f, 1.0f, 0.1f, 1.0f));
    EXPECT_EQ(emitter->initialColor.max, emitter->initialColor.min);
    EXPECT_FLOAT_EQ(emitter->initialSize.min, 0.1f);
    EXPECT_FLOAT_EQ(emitter->initialSize.max, 0.3f);

    // Seed/tick determinism belongs to ParticleEcsRuntimeGatewayValidation,
    // where the exact atlas publication view is available. This cook/load test
    // verifies the authored runtime ranges without manufacturing a render-ready
    // texture handle or bypassing the GFX17 publication contract.
    const auto& effect = root->GetSystem()->renderEffect;
    EXPECT_TRUE(effect.flipbook.required);
    EXPECT_EQ(effect.flipbook.textureAssetId.value, root->GetAtlasTextureResourceId());
    EXPECT_FALSE(effect.flipbook.texture.IsValid());
    EXPECT_NE(effect.contentHash, 0u);

    RVX::uint32 cancellationPolls = 0u;
    context.isCancellationRequested = [&cancellationPolls]()
    {
        return ++cancellationPolls >= 3u;
    };
    RVX::Resource::PreparedResourceBundle cancelledBundle;
    RVX::Resource::ResourceLoadError cancelledError;
    EXPECT_FALSE(loader.Prepare(context, cancelledBundle, cancelledError));
    EXPECT_EQ(cancelledError.code, RVX::Resource::ResourceLoadErrorCode::Cancelled);
    EXPECT_GE(cancellationPolls, 3u);
    context.isCancellationRequested = {};

    std::vector<RVX::uint8> tamperedAtlas = ReadBytes(atlasPath);
    ASSERT_FALSE(tamperedAtlas.empty());
    tamperedAtlas.back() ^= 0x80u;
    ASSERT_TRUE(WriteBytes(atlasPath, tamperedAtlas));
    RVX::Resource::PreparedResourceBundle tamperedBundle;
    RVX::Resource::ResourceLoadError tamperedError;
    EXPECT_FALSE(loader.Prepare(context, tamperedBundle, tamperedError));
    EXPECT_NE(tamperedError.message.find("hash"), std::string::npos);
#endif
}

TEST_F(ParticleEffectCookPipelineValidation,
     CookDirectoryOrdersAtlasBeforeLexicallyEarlierEffectAndRecordsManifestDependencyOutputs)
{
    ScopedTemporaryDirectory fixture;
    const fs::path source = fixture.Get() / "Source";
    const fs::path cooked = fixture.Get() / "Cooked";

    // 00_Fire.rvxparticle sorts before 99_Fire.tga. The explicit two-phase
    // contract must produce the texture sidecar before the effect importer
    // binds its content identity.
    ASSERT_TRUE(WriteRgbaTga(source / "Vfx" / "99_Fire.tga"));
    ASSERT_TRUE(WriteText(source / "Vfx" / "00_Fire.rvxparticle", ValidAuthoringJson()));

    RVX::Tools::AssetPipeline pipeline;
    pipeline.RegisterImporter(std::make_unique<RVX::Tools::TextureImporter>());
    pipeline.RegisterImporter(std::make_unique<RVX::Tools::ParticleEffectImporter>());
    const RVX::Tools::CookManifest manifest = pipeline.CookDirectory(
        source, cooked, true, cooked / "CookManifest.rvxmanifest");
    ASSERT_TRUE(manifest.manifestError.empty()) << manifest.manifestError;
    ASSERT_EQ(manifest.GetFailureCount(), 0u);

    const auto effectEntry = std::find_if(
        manifest.entries.begin(), manifest.entries.end(),
        [](const RVX::Tools::CookManifestEntry& entry)
        {
            return entry.type == RVX::Tools::AssetType::Effect;
        });
    ASSERT_NE(effectEntry, manifest.entries.end());
    EXPECT_EQ(effectEntry->sourcePath, "Vfx/00_Fire.rvxparticle");
    EXPECT_EQ(effectEntry->outputPath, "Vfx/00_Fire.rvxvfx");
    ASSERT_EQ(effectEntry->artifacts.size(), 1u);
    EXPECT_EQ(effectEntry->artifacts[0].relativePath, "Vfx/00_Fire.rvxvfx");
    EXPECT_FALSE(std::any_of(effectEntry->artifacts.begin(), effectEntry->artifacts.end(),
        [](const RVX::Tools::CookContentIdentity& artifact)
        {
            return artifact.relativePath == "Vfx/99_Fire.rva";
        }));
    ASSERT_EQ(effectEntry->dependencies.size(), 1u);
    EXPECT_EQ(effectEntry->dependencies[0].relativePath, "Vfx/99_Fire.rva");
    EXPECT_EQ(effectEntry->dependencyOutputs,
              std::vector<std::string>({"Vfx/99_Fire.rva"}));

    RVX::Resource::CookedParticleEffectArtifact artifact;
    std::string error;
    ASSERT_TRUE(RVX::Resource::DeserializeCookedParticleEffectArtifact(
        ReadBytes(cooked / "Vfx" / "00_Fire.rvxvfx"), artifact, error)) << error;
    EXPECT_EQ(effectEntry->dependencies[0].sha256,
              RVX::Hash::FormatSHA256Digest(artifact.flipbook.expectedContentHash));

    const size_t atlasArtifactOwnerCount = static_cast<size_t>(std::count_if(
        manifest.entries.begin(), manifest.entries.end(),
        [](const RVX::Tools::CookManifestEntry& entry)
        {
            return std::any_of(entry.artifacts.begin(), entry.artifacts.end(),
                [](const RVX::Tools::CookContentIdentity& artifact)
                {
                    return artifact.relativePath == "Vfx/99_Fire.rva";
                });
        }));
    EXPECT_EQ(atlasArtifactOwnerCount, 1u);
}

TEST_F(ParticleEffectCookPipelineValidation,
     CookedRuntimeResolvesUnschemedEffectAndRetainsSharedBoundAtlas)
{
#ifndef RVX_COOK_EXECUTABLE_PATH
    GTEST_SKIP() << "RVXCook executable path was not provided by CMake";
#else
    ScopedTemporaryDirectory fixture;
    const fs::path source = fixture.Get() / "Source";
    const fs::path cooked = fixture.Get() / "Cooked";
    ASSERT_TRUE(WriteRgbaTga(source / "Vfx" / "99_Fire.tga"));
    ASSERT_TRUE(WriteText(source / "Vfx" / "00_Fire.rvxparticle", ValidAuthoringJson()));
    ASSERT_EQ(RunCook(source, cooked), 0);

    const fs::path firstEffect = cooked / "Vfx" / "00_Fire.rvxvfx";
    const fs::path secondEffect = cooked / "Vfx" / "01_Fire.rvxvfx";
    std::error_code copyError;
    fs::copy_file(firstEffect, secondEffect, fs::copy_options::overwrite_existing, copyError);
    ASSERT_FALSE(copyError) << copyError.message();

    RVX::Resource::ResourceManagerConfig config;
    config.asyncThreadCount = 0;
    config.runtimePolicy.mode = RVX::Resource::ResourceRuntimeMode::CookedRuntime;
    config.runtimePolicy.allowSourceAssetReads = false;
    config.runtimePolicy.requireCookedArtifacts = true;
    config.runtimePolicy.cookedRoot = cooked.string();
    RVX::Resource::ResourceManagerDependencies dependencies =
        RVX::Resource::CreateRuntimeResourceManagerDependencies();
    std::string catalogError;
    ASSERT_TRUE(dependencies.loaderCatalog.Add(
        std::make_unique<RVX::Particle::ParticleSystemLoader>(), catalogError)) << catalogError;

    RVX::Resource::ResourceManager manager;
    manager.Initialize(config, std::move(dependencies));
    ASSERT_TRUE(manager.IsInitialized());
    auto* first = dynamic_cast<RVX::Particle::ParticleSystemResource*>(
        manager.LoadResource("Vfx/00_Fire.rvxvfx", RVX::Particle::ResourceType_ParticleSystem));
    auto* second = dynamic_cast<RVX::Particle::ParticleSystemResource*>(
        manager.LoadResource("Vfx/01_Fire.rvxvfx", RVX::Particle::ResourceType_ParticleSystem));
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    EXPECT_EQ(manager.GetLastLoadDiagnostic().domain,
              RVX::Resource::ResourceLoadDomain::CookedArtifact);
    const RVX::Resource::ResourceId atlasId = first->GetAtlasTextureResourceId();
    EXPECT_NE(atlasId, RVX::Resource::InvalidResourceId);
    EXPECT_EQ(second->GetAtlasTextureResourceId(), atlasId);
    EXPECT_NE(first->GetAtlasDependencyIdentity().find("sha256="), std::string::npos);
    EXPECT_EQ(first->GetRequiredDependencies(),
              std::vector<RVX::Resource::ResourceId>({atlasId}));
    EXPECT_EQ(second->GetRequiredDependencies(),
              std::vector<RVX::Resource::ResourceId>({atlasId}));
    const auto canonicalAtlas = manager.TryAcquireLoaded<
        RVX::Resource::TextureResource>(atlasId);
    ASSERT_TRUE(canonicalAtlas);
    EXPECT_TRUE(canonicalAtlas->IsLoaded());

    const auto firstKey = manager.FindPublishedAssetKey(first->GetId());
    const auto secondKey = manager.FindPublishedAssetKey(second->GetId());
    ASSERT_TRUE(firstKey.has_value());
    ASSERT_TRUE(secondKey.has_value());
    EXPECT_EQ(manager.Unload(*firstKey), RVX::Resource::AssetResidencyReleaseResult::Unloaded);
    EXPECT_FALSE(manager.IsLoaded(*firstKey));
    EXPECT_TRUE(manager.IsLoaded(*secondKey));
    {
        const auto survivingEffect = manager.TryAcquireLoaded<
            RVX::Particle::ParticleSystemResource>(second->GetId());
        const auto survivingAtlas = manager.TryAcquireLoaded<
            RVX::Resource::TextureResource>(atlasId);
        ASSERT_TRUE(survivingEffect);
        ASSERT_TRUE(survivingAtlas);
        EXPECT_EQ(survivingEffect->GetAtlasTextureResourceId(), survivingAtlas.GetId());
    }
    manager.Shutdown();
#endif
}

TEST_F(ParticleEffectCookPipelineValidation, RejectsUnsupportedOrUnknownAuthoring)
{
#ifndef RVX_COOK_EXECUTABLE_PATH
    GTEST_SKIP() << "RVXCook executable path was not provided by CMake";
#else
    ScopedTemporaryDirectory fixture;
    const fs::path source = fixture.Get() / "Source";
    const fs::path output = fixture.Get() / "Cooked";
    ASSERT_TRUE(WriteText(source / "Bad.rvxparticle",
        R"json({"schema":"RVX_PARTICLE_EFFECT_AUTHORING_V1","logicalAssetId":"Effects/Bad","render":{"simulation":"GPU","renderMode":"Billboard","blendMode":"Additive"}})json"));
    EXPECT_NE(RunCook(source, output), 0);
    EXPECT_FALSE(fs::exists(output / "Bad.rvxvfx"));
#endif
}

TEST_F(ParticleEffectCookPipelineValidation, RejectsEffectWhoseCookedAtlasWasNotProduced)
{
#ifndef RVX_COOK_EXECUTABLE_PATH
    GTEST_SKIP() << "RVXCook executable path was not provided by CMake";
#else
    ScopedTemporaryDirectory fixture;
    const fs::path source = fixture.Get() / "Source";
    const fs::path output = fixture.Get() / "Cooked";
    ASSERT_TRUE(WriteText(source / "Vfx" / "99_Fire.rvxparticle", ValidAuthoringJson()));

    // The authoring reference is a cooked sidecar, never a source fallback.
    // Its absence aborts the whole cook transaction before publication.
    EXPECT_NE(RunCook(source, output), 0);
    EXPECT_FALSE(fs::exists(output / "Vfx" / "99_Fire.rvxvfx"));
#endif
}

TEST_F(ParticleEffectCookPipelineValidation,
     CookerRejectsMalformedWrongUsageAndNonIntegralFlipbookAtlases)
{
    ScopedTemporaryDirectory fixture;
    RVX::Tools::ParticleEffectImporter importer;
    const auto importWithAtlas = [&](const std::string& name,
                                     const std::function<bool(const fs::path&)>& writeAtlas)
    {
        const fs::path root = fixture.Get() / name;
        const fs::path sourcePath = root / "Source.rvxparticle";
        const fs::path outputPath = root / "Cooked" / "00_Fire.rvxvfx";
        if (!WriteText(sourcePath, ValidAuthoringJson()))
        {
            ADD_FAILURE() << "Could not write particle-effect authoring fixture";
            return std::string{};
        }
        if (!writeAtlas(outputPath.parent_path() / "99_Fire.rva"))
        {
            ADD_FAILURE() << "Could not write cooked atlas fixture";
            return std::string{};
        }
        const RVX::Tools::ImportResult result = importer.Import(sourcePath, outputPath);
        EXPECT_FALSE(result.success) << result.error;
        EXPECT_FALSE(fs::exists(outputPath));
        return result.error;
    };

    const std::string malformed = importWithAtlas("Malformed",
        [](const fs::path& atlas)
        {
            return WriteText(atlas, "not a cooked texture");
        });
    EXPECT_NE(malformed.find("invalid"), std::string::npos);

    const std::string wrongUsage = importWithAtlas("WrongUsage",
        [](const fs::path& atlas)
        {
            return WriteCookedRva(atlas, 4u, 4u, "Normal");
        });
    EXPECT_NE(wrongUsage.find("Color"), std::string::npos);

    const std::string wrongDimensions = importWithAtlas("WrongDimensions",
        [](const fs::path& atlas)
        {
            return WriteCookedRva(atlas, 5u, 4u, "Color");
        });
    EXPECT_NE(wrongDimensions.find("tile grid"), std::string::npos);
}
