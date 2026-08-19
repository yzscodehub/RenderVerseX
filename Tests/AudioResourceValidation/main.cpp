#include "Core/Log.h"
#include "Resource/Loader/AudioLoader.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

namespace
{
    void WriteU16(std::ofstream& file, std::uint16_t value)
    {
        file.put(static_cast<char>(value & 0xFFu));
        file.put(static_cast<char>((value >> 8u) & 0xFFu));
    }

    void WriteU32(std::ofstream& file, std::uint32_t value)
    {
        file.put(static_cast<char>(value & 0xFFu));
        file.put(static_cast<char>((value >> 8u) & 0xFFu));
        file.put(static_cast<char>((value >> 16u) & 0xFFu));
        file.put(static_cast<char>((value >> 24u) & 0xFFu));
    }

    std::filesystem::path WriteSilentWav(const std::string& fileName)
    {
        const std::filesystem::path path = std::filesystem::temp_directory_path() / fileName;
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        if (!file)
        {
            return {};
        }

        constexpr std::uint16_t channels = 1;
        constexpr std::uint32_t sampleRate = 8000;
        constexpr std::uint16_t bitsPerSample = 16;
        constexpr std::uint32_t frameCount = 8;
        constexpr std::uint16_t blockAlign = channels * bitsPerSample / 8;
        constexpr std::uint32_t byteRate = sampleRate * blockAlign;
        constexpr std::uint32_t dataSize = frameCount * blockAlign;

        file.write("RIFF", 4);
        WriteU32(file, 36u + dataSize);
        file.write("WAVE", 4);
        file.write("fmt ", 4);
        WriteU32(file, 16u);
        WriteU16(file, 1u);
        WriteU16(file, channels);
        WriteU32(file, sampleRate);
        WriteU32(file, byteRate);
        WriteU16(file, blockAlign);
        WriteU16(file, bitsPerSample);
        file.write("data", 4);
        WriteU32(file, dataSize);
        for (std::uint32_t i = 0; i < frameCount; ++i)
        {
            WriteU16(file, 0u);
        }

        return path;
    }

    void RemoveTempFile(const std::filesystem::path& path)
    {
        std::error_code error;
        std::filesystem::remove(path, error);
    }

    struct LogScope
    {
        LogScope()
        {
            static bool initializedOnce = false;
            if (!initializedOnce && !RVX::Log::GetCoreLogger())
            {
                RVX::Log::Initialize();
            }
            initializedOnce = true;
        }
    };
} // namespace

TEST(AudioResourceValidation, StreamingRequestFailsWithUnsupportedDiagnostic)
{
    LogScope logScope;
    const std::filesystem::path wavPath = WriteSilentWav("rvx_audio_resource_streaming_unsupported.wav");
    ASSERT_FALSE(wavPath.empty());

    RVX::Resource::AudioLoader loader(nullptr);
    RVX::Resource::AudioLoadOptions options;
    options.enableStreaming = true;

    RVX::Resource::AudioResource* audio = loader.LoadWithOptions(wavPath.string(), options);
    EXPECT_EQ(nullptr, audio);
    EXPECT_EQ(RVX::Resource::AudioLoadStatus::UnsupportedStreaming, loader.GetLastLoadStatus());
    EXPECT_FALSE(loader.GetLastLoadError().empty());
    EXPECT_NE(std::string::npos, loader.GetLastLoadError().find("unsupported"));

    RemoveTempFile(wavPath);
}

TEST(AudioResourceValidation, ThresholdFallbackLoadsFullyWithoutFakeStreaming)
{
    LogScope logScope;
    const std::filesystem::path wavPath = WriteSilentWav("rvx_audio_resource_threshold_fallback.wav");
    ASSERT_FALSE(wavPath.empty());

    RVX::Resource::AudioLoader loader(nullptr);
    RVX::Resource::AudioLoadOptions options;
    options.streamingThreshold = 1;

    RVX::Resource::AudioResource* audio = loader.LoadWithOptions(wavPath.string(), options);
    ASSERT_NE(nullptr, audio) << loader.GetLastLoadError();
    EXPECT_EQ(RVX::Resource::AudioLoadStatus::FallbackStreamingUnsupported, loader.GetLastLoadStatus());
    EXPECT_TRUE(loader.WasLastLoadFallback());
    EXPECT_FALSE(loader.GetLastLoadError().empty());
    EXPECT_NE(std::string::npos, loader.GetLastLoadError().find("unsupported"));
    EXPECT_TRUE(audio->IsLoaded());
    EXPECT_FALSE(audio->IsStreaming());
    EXPECT_EQ(RVX::Resource::AudioLoadMode::FullyLoaded, audio->GetLoadMode());
    EXPECT_GT(audio->GetDataSize(), 0u);
    EXPECT_TRUE(audio->GetStreamingSourcePath().empty());

    delete audio;
    RemoveTempFile(wavPath);
}
