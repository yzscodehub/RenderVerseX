#include "Core/Log.h"
#include "Resource/CookManifest.h"
#include "Resource/Cooked/CookedAudioArtifact.h"
#include "Resource/Cooked/CookedEnvironmentArtifact.h"
#include "Resource/Loader/CookedAudioLoader.h"
#include "Resource/Loader/CookedEnvironmentLoader.h"
#include "Resource/ResourceManager.h"
#include "Tools/AssetPipeline.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

namespace
{
namespace fs = std::filesystem;
using namespace RVX;

class ScopedTempDirectory final
{
public:
    explicit ScopedTempDirectory(const char* label)
    {
        const auto ticks =
            std::chrono::steady_clock::now().time_since_epoch().count();
        m_path = fs::temp_directory_path() /
            (std::string("RVX_CookedEnvironmentAudio_") + label + '_' +
             std::to_string(ticks));
        std::error_code error;
        fs::create_directories(m_path, error);
        EXPECT_FALSE(error) << error.message();
    }

    ~ScopedTempDirectory()
    {
        std::error_code error;
        fs::remove_all(m_path, error);
    }

    const fs::path& Get() const { return m_path; }

private:
    fs::path m_path;
};

std::vector<uint8> ReadBytes(const fs::path& path)
{
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), {}};
}

template<typename Loader>
Resource::ResourceHandle<Resource::IResource> PrepareCookedResource(
    Loader& loader,
    const fs::path& path,
    Resource::ResourceType type,
    std::string& outError)
{
    Resource::ResourceLoadPreparationContext context;
    context.requestedPath = path.string();
    context.resolvedPath = path.string();
    context.assetKey = Resource::MakeAssetKey(context.requestedPath, type);
    context.resourceIdentityPath = context.assetKey.canonicalPath;
    context.rootResourceId = Resource::GenerateResourceId(
        context.resourceIdentityPath);

    Resource::PreparedResourceBundle bundle;
    Resource::ResourceLoadError error;
    if (!loader.Prepare(context, bundle, error))
    {
        outError = error.message;
        return {};
    }

    std::vector<Resource::IResource*> resources;
    resources.reserve(bundle.GetEntries().size());
    for (const Resource::PreparedResourceEntry& entry : bundle.GetEntries())
        resources.push_back(entry.resource.Get());
    Resource::ResourceCache publicationCache;
    if (!publicationCache.StorePreparedBatch(resources))
    {
        outError = "Could not publish prepared cooked resource bundle.";
        return {};
    }
    return bundle.GetRoot();
}

void WriteU16(std::ofstream& output, uint16 value)
{
    const uint8 bytes[] = {
        static_cast<uint8>(value), static_cast<uint8>(value >> 8u)};
    output.write(reinterpret_cast<const char*>(bytes), sizeof(bytes));
}

void WriteU32(std::ofstream& output, uint32 value)
{
    const uint8 bytes[] = {
        static_cast<uint8>(value), static_cast<uint8>(value >> 8u),
        static_cast<uint8>(value >> 16u), static_cast<uint8>(value >> 24u)};
    output.write(reinterpret_cast<const char*>(bytes), sizeof(bytes));
}

bool WriteWaveFixture(const fs::path& path)
{
    constexpr uint32 sampleRate = 8000;
    constexpr uint16 channels = 1;
    constexpr uint32 frameCount = 128;
    constexpr uint32 dataSize = frameCount * sizeof(int16);
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) return false;
    output.write("RIFF", 4);
    WriteU32(output, 36u + dataSize);
    output.write("WAVEfmt ", 8);
    WriteU32(output, 16);
    WriteU16(output, 1);
    WriteU16(output, channels);
    WriteU32(output, sampleRate);
    WriteU32(output, sampleRate * channels * sizeof(int16));
    WriteU16(output, channels * sizeof(int16));
    WriteU16(output, 16);
    output.write("data", 4);
    WriteU32(output, dataSize);
    for (uint32 frame = 0; frame < frameCount; ++frame)
    {
        const float phase = static_cast<float>(frame) * 0.15f;
        WriteU16(output, static_cast<uint16>(
            static_cast<int16>(std::sin(phase) * 12000.0f)));
    }
    return output.good();
}

void WriteRGBE(std::ofstream& output, float red, float green, float blue)
{
    const float maximum = std::max({red, green, blue});
    uint8 values[4]{};
    if (maximum > 0.0f)
    {
        int exponent = 0;
        const float mantissa = std::frexp(maximum, &exponent);
        const float scale = mantissa * 256.0f / maximum;
        values[0] = static_cast<uint8>(std::clamp(red * scale, 0.0f, 255.0f));
        values[1] = static_cast<uint8>(std::clamp(green * scale, 0.0f, 255.0f));
        values[2] = static_cast<uint8>(std::clamp(blue * scale, 0.0f, 255.0f));
        values[3] = static_cast<uint8>(exponent + 128);
    }
    output.write(reinterpret_cast<const char*>(values), sizeof(values));
}

bool WriteHdrFixture(const fs::path& path)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) return false;
    constexpr uint32 width = 4;
    constexpr uint32 height = 2;
    output << "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n";
    output << "-Y " << height << " +X " << width << '\n';
    for (uint32 y = 0; y < height; ++y)
    {
        for (uint32 x = 0; x < width; ++x)
        {
            WriteRGBE(output, 0.25f + static_cast<float>(x) * 0.1f,
                      0.5f + static_cast<float>(y) * 0.1f, 0.75f);
        }
    }
    return output.good();
}

Resource::CookedEnvironmentArtifact MakeEnvironmentArtifact()
{
    Resource::CookedEnvironmentArtifact artifact;
    artifact.sourcePath = "Environment/Fixture.hdr";
    artifact.importOptionsHash = 1234;
    artifact.intensity = 1.25f;
    for (size_t index = 0; index < artifact.textures.size(); ++index)
    {
        auto& texture = artifact.textures[index];
        texture.role =
            static_cast<Resource::CookedEnvironmentTextureRole>(index);
        texture.width = index == 2 ? 2u : 1u;
        texture.height = index == 2 ? 2u : 1u;
        texture.mipLevels = index == 2 ? 2u : 1u;
        texture.cubemap = index != 3;
        texture.arrayLayers = texture.cubemap ? 6u : 1u;
        size_t texelCount = 0;
        for (uint32 mip = 0; mip < texture.mipLevels; ++mip)
        {
            texelCount += std::max(1u, texture.width >> mip) *
                std::max(1u, texture.height >> mip) * texture.arrayLayers;
        }
        texture.rgba16f.resize(texelCount * 4u,
                               static_cast<uint16>(0x3c00u + index));
    }
    return artifact;
}

void EnsureLoggingInitialized()
{
    if (!Log::GetCoreLogger())
        Log::Initialize();
}
} // namespace

TEST(CookedEnvironmentAudioArtifactValidation,
     AudioRoundTripIsDeterministicAndRejectsTampering)
{
    Resource::CookedAudioArtifact artifact;
    artifact.sourcePath = "Audio/Fixture.wav";
    artifact.sampleRate = 48000;
    artifact.channels = 2;
    artifact.frameCount = 3;
    artifact.looping = true;
    artifact.loopStartFrame = 1;
    artifact.loopEndFrame = 3;
    artifact.pcm16 = {-32768, 32767, -1024, 1024, 0, 42};

    std::vector<uint8> first;
    std::vector<uint8> second;
    std::string error;
    ASSERT_TRUE(Resource::SerializeCookedAudioArtifact(
        artifact, first, error)) << error;
    ASSERT_TRUE(Resource::SerializeCookedAudioArtifact(
        artifact, second, error)) << error;
    EXPECT_EQ(first, second);

    Resource::CookedAudioArtifact parsed;
    ASSERT_TRUE(Resource::DeserializeCookedAudioArtifact(
        first, parsed, error)) << error;
    EXPECT_EQ(parsed.pcm16, artifact.pcm16);
    EXPECT_EQ(parsed.frameCount, artifact.frameCount);
    EXPECT_EQ(parsed.loopEndFrame, artifact.loopEndFrame);

    first.back() ^= 0x80u;
    EXPECT_FALSE(Resource::DeserializeCookedAudioArtifact(
        first, parsed, error));
    EXPECT_NE(error.find("hash"), std::string::npos);
}

TEST(CookedEnvironmentAudioArtifactValidation,
     EnvironmentRoundTripIsDeterministicAndRejectsTampering)
{
    const Resource::CookedEnvironmentArtifact artifact =
        MakeEnvironmentArtifact();
    std::vector<uint8> first;
    std::vector<uint8> second;
    std::string error;
    ASSERT_TRUE(Resource::SerializeCookedEnvironmentArtifact(
        artifact, first, error)) << error;
    ASSERT_TRUE(Resource::SerializeCookedEnvironmentArtifact(
        artifact, second, error)) << error;
    EXPECT_EQ(first, second);

    Resource::CookedEnvironmentArtifact parsed;
    ASSERT_TRUE(Resource::DeserializeCookedEnvironmentArtifact(
        first, parsed, error)) << error;
    EXPECT_EQ(parsed.textures[2].mipLevels, 2u);
    EXPECT_EQ(parsed.textures[2].arrayLayers, 6u);
    EXPECT_EQ(parsed.textures[3].arrayLayers, 1u);

    first.back() ^= 0x01u;
    EXPECT_FALSE(Resource::DeserializeCookedEnvironmentArtifact(
        first, parsed, error));
    EXPECT_NE(error.find("hash"), std::string::npos);
}

TEST(CookedEnvironmentAudioArtifactValidation,
     RuntimeCatalogRejectsSourceDomainAndAcceptsCookedDomain)
{
    Resource::ResourceManagerDependencies dependencies =
        Resource::CreateRuntimeResourceManagerDependencies();
    std::string sealError;
    ASSERT_TRUE(dependencies.loaderCatalog.Seal(sealError)) << sealError;
    const auto sourceAudio = dependencies.loaderCatalog.Resolve({
        Resource::ResourceType::Audio,
        Resource::ResourceLoadDomain::Source,
        "Audio/Fixture.wav"});
    const auto sourceEnvironment = dependencies.loaderCatalog.Resolve({
        Resource::ResourceType::Environment,
        Resource::ResourceLoadDomain::Source,
        "Environment/Fixture.hdr"});
    EXPECT_EQ(sourceAudio.status,
              Resource::ResourceLoaderResolveStatus::NotFound);
    EXPECT_EQ(sourceEnvironment.status,
              Resource::ResourceLoaderResolveStatus::NotFound);

    EXPECT_TRUE(dependencies.loaderCatalog.Resolve({
        Resource::ResourceType::Audio,
        Resource::ResourceLoadDomain::Cooked,
        "Audio/Fixture.rva"}).Succeeded());
    EXPECT_TRUE(dependencies.loaderCatalog.Resolve({
        Resource::ResourceType::Environment,
        Resource::ResourceLoadDomain::Package,
        "Environment/Fixture.rva"}).Succeeded());
}

TEST(CookedEnvironmentAudioArtifactValidation,
     DirectoryCookProducesRuntimeReadableDeterministicProductsAndManifest)
{
    EnsureLoggingInitialized();
    ScopedTempDirectory fixture("DirectoryCook");
    const fs::path source = fixture.Get() / "Source";
    const fs::path outputA = fixture.Get() / "CookedA";
    const fs::path outputB = fixture.Get() / "CookedB";
    ASSERT_TRUE(fs::create_directories(source));
    ASSERT_TRUE(WriteWaveFixture(source / "Tone.wav"));
    ASSERT_TRUE(WriteHdrFixture(source / "Sky.hdr"));

    Tools::EnvironmentImportOptions environmentOptions;
    environmentOptions.quality = 0;
    Tools::AudioCookOptions audioOptions;
    const auto cook = [&](const fs::path& output)
    {
        Tools::AssetPipeline pipeline;
        pipeline.RegisterImporter(std::make_unique<Tools::AudioImporter>());
        pipeline.RegisterImporter(std::make_unique<Tools::EnvironmentImporter>());
        return pipeline.CookDirectory(
            source, output, true, output / "CookManifest.rvxmanifest", {},
            [&](const fs::path&, Tools::AssetType type) -> const void*
            {
                if (type == Tools::AssetType::Environment)
                    return &environmentOptions;
                if (type == Tools::AssetType::Audio)
                    return &audioOptions;
                return nullptr;
            });
    };

    const Tools::CookManifest first = cook(outputA);
    const Tools::CookManifest second = cook(outputB);
    ASSERT_TRUE(first.manifestWritten) << first.manifestError;
    ASSERT_TRUE(second.manifestWritten) << second.manifestError;
    ASSERT_EQ(first.GetFailureCount(), 0u);
    ASSERT_EQ(second.GetFailureCount(), 0u);
    EXPECT_EQ(ReadBytes(outputA / "Tone.rva"),
              ReadBytes(outputB / "Tone.rva"));
    EXPECT_EQ(ReadBytes(outputA / "Sky.rva"),
              ReadBytes(outputB / "Sky.rva"));

    Resource::CookManifest runtimeManifest;
    std::string error;
    ASSERT_TRUE(Resource::LoadCookManifest(
        outputA / "CookManifest.rvxmanifest", runtimeManifest, error)) << error;
    ASSERT_EQ(runtimeManifest.entries.size(), 2u);

    Resource::CookedAudioLoader audioLoader;
    Resource::ResourceHandle<Resource::IResource> audio =
        PrepareCookedResource(
            audioLoader,
            outputA / "Tone.rva",
            Resource::ResourceType::Audio,
            error);
    ASSERT_TRUE(audio) << error;
    EXPECT_EQ(audio->GetType(), Resource::ResourceType::Audio);
    EXPECT_TRUE(audio->IsLoaded());

    Resource::CookedEnvironmentLoader environmentLoader;
    Resource::ResourceHandle<Resource::IResource> environment =
        PrepareCookedResource(
            environmentLoader,
            outputA / "Sky.rva",
            Resource::ResourceType::Environment,
            error);
    ASSERT_TRUE(environment) << error;
    EXPECT_EQ(environment->GetType(), Resource::ResourceType::Environment);
    EXPECT_TRUE(environment->IsLoaded());
}
