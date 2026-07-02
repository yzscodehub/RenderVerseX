#include "Audio/AudioEngine.h"
#include "Audio/AudioSubsystem.h"
#include "Audio/DSP/LowPassEffect.h"
#include "Audio/DSP/ReverbEffect.h"
#include "Audio/Events/AudioEvent.h"
#include "Audio/Mixer/AudioBus.h"
#include "Audio/Mixer/AudioMixer.h"
#include "Audio/Spatial/AudioZone.h"
#include "Audio/Spatial/AudioZoneManager.h"
#include "Audio/Spatial/IOcclusionProvider.h"
#include "Audio/Streaming/AudioStreamer.h"
#include "Core/Log.h"
#include "Physics/PhysicsWorld.h"
#include "Physics/Shapes/CollisionShape.h"
#include "Scene/Components/AudioComponent.h"
#include "Scene/SceneEntity.h"

#include <gtest/gtest.h>
#include <cstdint>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>

namespace
{
    RVX::Physics::BodyHandle CreateBoxBody(RVX::Physics::PhysicsWorld& physicsWorld,
                                           const RVX::Vec3& position,
                                           RVX::Physics::CollisionLayer layer = RVX::Physics::Layers::Static)
    {
        RVX::Physics::RigidBodyDesc desc;
        desc.type = RVX::Physics::BodyType::Static;
        desc.position = position;
        desc.layer = layer;
        desc.collisionMask = 0xFFFFFFFFu;

        const RVX::Physics::BodyHandle handle = physicsWorld.CreateBody(desc);
        physicsWorld.AddShape(handle, RVX::Physics::BoxShape::Create(RVX::Vec3(0.5f)));
        return handle;
    }

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

    std::filesystem::path WriteSineWav(const std::string& fileName,
                                       std::uint32_t sampleRate,
                                       float frequency,
                                       std::uint32_t frameCount)
    {
        const std::filesystem::path path = std::filesystem::temp_directory_path() / fileName;
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        if (!file)
        {
            return {};
        }

        constexpr std::uint16_t channels = 1;
        constexpr std::uint16_t bitsPerSample = 16;
        constexpr std::uint16_t blockAlign = channels * bitsPerSample / 8;
        const std::uint32_t byteRate = sampleRate * blockAlign;
        const std::uint32_t dataSize = frameCount * blockAlign;

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

        constexpr float pi = 3.14159265358979323846f;
        for (std::uint32_t i = 0; i < frameCount; ++i)
        {
            const float phase = 2.0f * pi * frequency * static_cast<float>(i) / static_cast<float>(sampleRate);
            const float sample = std::sin(phase) * 0.75f;
            const auto signedSample = static_cast<std::int16_t>(sample * 32767.0f);
            WriteU16(file, static_cast<std::uint16_t>(signedSample));
        }

        return path;
    }

    std::filesystem::path WriteStereoImpulseWav(const std::string& fileName,
                                                std::uint32_t sampleRate,
                                                std::uint32_t frameCount)
    {
        const std::filesystem::path path = std::filesystem::temp_directory_path() / fileName;
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        if (!file)
        {
            return {};
        }

        constexpr std::uint16_t channels = 2;
        constexpr std::uint16_t bitsPerSample = 16;
        constexpr std::uint16_t blockAlign = channels * bitsPerSample / 8;
        const std::uint32_t byteRate = sampleRate * blockAlign;
        const std::uint32_t dataSize = frameCount * blockAlign;

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
            const std::int16_t sample = i == 0 ? static_cast<std::int16_t>(24000) : 0;
            WriteU16(file, static_cast<std::uint16_t>(sample));
            WriteU16(file, static_cast<std::uint16_t>(sample));
        }

        return path;
    }

    float CalculateRms(const std::vector<float>& samples, RVX::uint64 frameCount)
    {
        if (frameCount == 0)
        {
            return 0.0f;
        }

        double sumSquares = 0.0;
        for (RVX::uint64 i = 0; i < frameCount; ++i)
        {
            const float sample = samples[static_cast<size_t>(i)];
            sumSquares += static_cast<double>(sample) * static_cast<double>(sample);
        }

        return static_cast<float>(std::sqrt(sumSquares / static_cast<double>(frameCount)));
    }

    float CalculateRmsRange(const std::vector<float>& samples, size_t beginSample, size_t endSample)
    {
        if (beginSample >= endSample || beginSample >= samples.size())
        {
            return 0.0f;
        }

        endSample = std::min(endSample, samples.size());

        double sumSquares = 0.0;
        for (size_t i = beginSample; i < endSample; ++i)
        {
            const float sample = samples[i];
            sumSquares += static_cast<double>(sample) * static_cast<double>(sample);
        }

        return static_cast<float>(std::sqrt(sumSquares / static_cast<double>(endSample - beginSample)));
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

TEST(AudioSpatialValidation, RaycastOcclusionDefaultsToUnoccludedWithoutPhysicsWorld)
{
    RVX::Audio::RaycastOcclusionProvider provider;

    const RVX::Audio::OcclusionResult result =
        provider.CalculateOcclusion(RVX::Vec3(-5.0f, 0.0f, 0.0f), RVX::Vec3(5.0f, 0.0f, 0.0f));

    EXPECT_FLOAT_EQ(0.0f, result.occlusion);
    EXPECT_FLOAT_EQ(0.0f, result.obstruction);
    EXPECT_FLOAT_EQ(1.0f, result.transmission);
    EXPECT_FLOAT_EQ(20000.0f, result.lowPassCutoff);
    EXPECT_FLOAT_EQ(1.0f, result.volumeScale);
}

TEST(AudioSpatialValidation, RaycastOcclusionUsesPhysicsWorldHitsAndLayerMask)
{
    RVX::Physics::PhysicsWorldConfig config;
    config.gravity = RVX::Vec3(0.0f);

    RVX::Physics::PhysicsWorld physicsWorld;
    ASSERT_TRUE(physicsWorld.Initialize(config));

    ASSERT_TRUE(CreateBoxBody(physicsWorld, RVX::Vec3(-1.5f, 0.0f, 0.0f)).IsValid());
    ASSERT_TRUE(CreateBoxBody(physicsWorld, RVX::Vec3(1.5f, 0.0f, 0.0f)).IsValid());

    RVX::Audio::RaycastOcclusionProvider provider;
    provider.SetPhysicsWorld(&physicsWorld);
    provider.SetOcclusionPerHit(0.4f);
    provider.SetLowPassReduction(1000.0f);

    const RVX::Audio::OcclusionResult blocked =
        provider.CalculateOcclusion(RVX::Vec3(-5.0f, 0.0f, 0.0f), RVX::Vec3(5.0f, 0.0f, 0.0f));

    EXPECT_NEAR(0.8f, blocked.occlusion, 0.0001f);
    EXPECT_NEAR(0.8f, blocked.obstruction, 0.0001f);
    EXPECT_NEAR(0.2f, blocked.transmission, 0.0001f);
    EXPECT_NEAR(18000.0f, blocked.lowPassCutoff, 0.0001f);
    EXPECT_NEAR(0.6f, blocked.volumeScale, 0.0001f);

    provider.SetLayerMask(1u << RVX::Physics::Layers::Trigger);
    const RVX::Audio::OcclusionResult filtered =
        provider.CalculateOcclusion(RVX::Vec3(-5.0f, 0.0f, 0.0f), RVX::Vec3(5.0f, 0.0f, 0.0f));

    EXPECT_FLOAT_EQ(0.0f, filtered.occlusion);
    EXPECT_FLOAT_EQ(1.0f, filtered.transmission);

    physicsWorld.Shutdown();
}

TEST(AudioSpatialValidation, AudioEngineUsesResourceManagerAndRoutesSoundsToBus)
{
    LogScope logScope;
    const std::filesystem::path wavPath = WriteSilentWav("rvx_audio_engine_bus_route.wav");
    ASSERT_FALSE(wavPath.empty());

    RVX::Audio::AudioEngineConfig config;
    config.enableDevice = false;
    config.bufferSizeFrames = 64;
    config.resourceManagerJobThreadCount = 0;

    RVX::Audio::AudioEngine engine;
    ASSERT_TRUE(engine.Initialize(config));
    EXPECT_TRUE(engine.GetStatistics().resourceManagerActive);

    const RVX::uint32 sfxBus = engine.CreateBus("SFX", 0);
    EXPECT_TRUE(engine.HasBus(sfxBus));
    EXPECT_EQ(RVX::Audio::BusId::SFX, sfxBus);
    EXPECT_EQ(sfxBus, engine.GetBusId("SFX"));
    EXPECT_EQ(RVX::RVX_INVALID_INDEX, engine.GetBusId("Missing"));
    EXPECT_EQ(1u, engine.GetBusCount());

    RVX::Audio::AudioClip::Ptr clip = engine.LoadClip(wavPath.string());
    ASSERT_TRUE(clip);
    EXPECT_GE(engine.GetStatistics().cachedClipFiles, 1u);

    RVX::Audio::AudioPlaySettings settings;
    settings.busId = sfxBus;
    settings.startPaused = true;

    const RVX::Audio::AudioHandle handle = engine.Play(clip, settings);
    ASSERT_TRUE(handle.IsValid());
    EXPECT_EQ(sfxBus, engine.GetSoundBus(handle));

    const RVX::Audio::AudioEngine::Statistics stats = engine.GetStatistics();
    EXPECT_EQ(1u, stats.activeVoices);
    EXPECT_EQ(1u, stats.routedVoices);
    EXPECT_EQ(1u, stats.busCount);
    EXPECT_TRUE(stats.resourceManagerActive);

    engine.Shutdown();
    RemoveTempFile(wavPath);
}

TEST(AudioSpatialValidation, AudioEngineLowPassNodeFiltersNoDeviceMixedOutput)
{
    LogScope logScope;
    const std::filesystem::path wavPath =
        WriteSineWav("rvx_audio_engine_low_pass_filter.wav", 8000u, 3000.0f, 2048u);
    ASSERT_FALSE(wavPath.empty());

    RVX::Audio::AudioEngineConfig config;
    config.enableDevice = false;
    config.sampleRate = 8000;
    config.channels = 1;
    config.bufferSizeFrames = 128;
    config.resourceManagerJobThreadCount = 0;

    RVX::Audio::AudioEngine dryEngine;
    ASSERT_TRUE(dryEngine.Initialize(config));
    RVX::Audio::AudioClip::Ptr dryClip = dryEngine.LoadClip(wavPath.string());
    ASSERT_TRUE(dryClip);
    const RVX::Audio::AudioHandle dryHandle = dryEngine.Play(dryClip);
    ASSERT_TRUE(dryHandle.IsValid());

    std::vector<float> dryFrames(512u, 0.0f);
    const RVX::uint64 dryRead = dryEngine.ReadMixedFrames(dryFrames.data(), static_cast<RVX::uint64>(dryFrames.size()));
    ASSERT_EQ(static_cast<RVX::uint64>(dryFrames.size()), dryRead);
    const float dryRms = CalculateRms(dryFrames, dryRead);
    EXPECT_GT(dryRms, 0.1f);

    RVX::Audio::AudioEngine filteredEngine;
    ASSERT_TRUE(filteredEngine.Initialize(config));
    RVX::Audio::AudioClip::Ptr filteredClip = filteredEngine.LoadClip(wavPath.string());
    ASSERT_TRUE(filteredClip);
    const RVX::Audio::AudioHandle filteredHandle = filteredEngine.Play(filteredClip);
    ASSERT_TRUE(filteredHandle.IsValid());
    filteredEngine.SetLowPassCutoff(filteredHandle, 500.0f);
    EXPECT_NEAR(500.0f, filteredEngine.GetSoundLowPassCutoff(filteredHandle), 0.0001f);

    std::vector<float> filteredFrames(512u, 0.0f);
    const RVX::uint64 filteredRead =
        filteredEngine.ReadMixedFrames(filteredFrames.data(), static_cast<RVX::uint64>(filteredFrames.size()));
    ASSERT_EQ(static_cast<RVX::uint64>(filteredFrames.size()), filteredRead);
    const float filteredRms = CalculateRms(filteredFrames, filteredRead);
    EXPECT_LT(filteredRms, dryRms * 0.65f);

    filteredEngine.Shutdown();
    dryEngine.Shutdown();
    RemoveTempFile(wavPath);
}

TEST(AudioSpatialValidation, AudioEngineBusLowPassNodeFiltersRoutedMixedOutput)
{
    LogScope logScope;
    const std::filesystem::path wavPath =
        WriteSineWav("rvx_audio_engine_bus_low_pass_filter.wav", 8000u, 3000.0f, 2048u);
    ASSERT_FALSE(wavPath.empty());

    RVX::Audio::AudioEngineConfig config;
    config.enableDevice = false;
    config.sampleRate = 8000;
    config.channels = 1;
    config.bufferSizeFrames = 128;
    config.resourceManagerJobThreadCount = 0;

    RVX::Audio::AudioEngine dryEngine;
    ASSERT_TRUE(dryEngine.Initialize(config));
    const RVX::uint32 dryAmbientBus = dryEngine.CreateBus("Ambient", RVX::Audio::BusId::Master);
    ASSERT_EQ(RVX::Audio::BusId::Ambient, dryAmbientBus);
    RVX::Audio::AudioClip::Ptr dryClip = dryEngine.LoadClip(wavPath.string());
    ASSERT_TRUE(dryClip);

    RVX::Audio::AudioPlaySettings drySettings;
    drySettings.busId = dryAmbientBus;
    const RVX::Audio::AudioHandle dryHandle = dryEngine.Play(dryClip, drySettings);
    ASSERT_TRUE(dryHandle.IsValid());
    EXPECT_EQ(dryAmbientBus, dryEngine.GetSoundBus(dryHandle));

    std::vector<float> dryFrames(512u, 0.0f);
    const RVX::uint64 dryRead = dryEngine.ReadMixedFrames(dryFrames.data(), static_cast<RVX::uint64>(dryFrames.size()));
    ASSERT_EQ(static_cast<RVX::uint64>(dryFrames.size()), dryRead);
    const float dryRms = CalculateRms(dryFrames, dryRead);
    EXPECT_GT(dryRms, 0.1f);

    RVX::Audio::AudioEngine filteredEngine;
    ASSERT_TRUE(filteredEngine.Initialize(config));
    const RVX::uint32 filteredAmbientBus = filteredEngine.CreateBus("Ambient", RVX::Audio::BusId::Master);
    ASSERT_EQ(RVX::Audio::BusId::Ambient, filteredAmbientBus);
    RVX::Audio::AudioClip::Ptr filteredClip = filteredEngine.LoadClip(wavPath.string());
    ASSERT_TRUE(filteredClip);

    RVX::Audio::AudioPlaySettings filteredSettings;
    filteredSettings.busId = filteredAmbientBus;
    const RVX::Audio::AudioHandle filteredHandle = filteredEngine.Play(filteredClip, filteredSettings);
    ASSERT_TRUE(filteredHandle.IsValid());
    filteredEngine.SetBusLowPassCutoff(filteredAmbientBus, 500.0f);
    EXPECT_NEAR(500.0f, filteredEngine.GetBusLowPassCutoff(filteredAmbientBus), 0.0001f);

    std::vector<float> filteredFrames(512u, 0.0f);
    const RVX::uint64 filteredRead =
        filteredEngine.ReadMixedFrames(filteredFrames.data(), static_cast<RVX::uint64>(filteredFrames.size()));
    ASSERT_EQ(static_cast<RVX::uint64>(filteredFrames.size()), filteredRead);
    const float filteredRms = CalculateRms(filteredFrames, filteredRead);
    EXPECT_LT(filteredRms, dryRms * 0.65f);

    filteredEngine.Shutdown();
    dryEngine.Shutdown();
    RemoveTempFile(wavPath);
}

TEST(AudioSpatialValidation, AudioEngineBusReverbNodeAddsTailToRoutedMixedOutput)
{
    LogScope logScope;
    constexpr RVX::uint32 sampleRate = 44100;
    constexpr RVX::uint64 frameCount = 4096;
    const std::filesystem::path wavPath =
        WriteStereoImpulseWav("rvx_audio_engine_bus_reverb.wav", sampleRate, static_cast<std::uint32_t>(frameCount));
    ASSERT_FALSE(wavPath.empty());

    RVX::Audio::AudioEngineConfig config;
    config.enableDevice = false;
    config.sampleRate = sampleRate;
    config.channels = 2;
    config.bufferSizeFrames = 256;
    config.resourceManagerJobThreadCount = 0;

    RVX::Audio::AudioEngine dryEngine;
    ASSERT_TRUE(dryEngine.Initialize(config));
    const RVX::uint32 dryAmbientBus = dryEngine.CreateBus("Ambient", RVX::Audio::BusId::Master);
    ASSERT_EQ(RVX::Audio::BusId::Ambient, dryAmbientBus);
    RVX::Audio::AudioClip::Ptr dryClip = dryEngine.LoadClip(wavPath.string());
    ASSERT_TRUE(dryClip);

    RVX::Audio::AudioPlaySettings drySettings;
    drySettings.busId = dryAmbientBus;
    ASSERT_TRUE(dryEngine.Play(dryClip, drySettings).IsValid());

    std::vector<float> dryFrames(static_cast<size_t>(frameCount * 2u), 0.0f);
    ASSERT_EQ(frameCount, dryEngine.ReadMixedFrames(dryFrames.data(), frameCount));
    const float dryTailRms = CalculateRmsRange(dryFrames, 1200u * 2u, dryFrames.size());
    EXPECT_LT(dryTailRms, 0.0001f);

    RVX::Audio::AudioEngine wetEngine;
    ASSERT_TRUE(wetEngine.Initialize(config));
    const RVX::uint32 wetAmbientBus = wetEngine.CreateBus("Ambient", RVX::Audio::BusId::Master);
    ASSERT_EQ(RVX::Audio::BusId::Ambient, wetAmbientBus);
    RVX::Audio::AudioClip::Ptr wetClip = wetEngine.LoadClip(wavPath.string());
    ASSERT_TRUE(wetClip);

    RVX::Audio::AudioPlaySettings wetSettings;
    wetSettings.busId = wetAmbientBus;
    ASSERT_TRUE(wetEngine.Play(wetClip, wetSettings).IsValid());

    RVX::Audio::ReverbSettings reverb;
    reverb.roomSize = 0.8f;
    reverb.damping = 0.3f;
    reverb.wetLevel = 0.8f;
    reverb.dryLevel = 0.2f;
    reverb.width = 1.0f;
    wetEngine.SetBusReverb(wetAmbientBus, reverb, true);
    EXPECT_TRUE(wetEngine.IsBusReverbEnabled(wetAmbientBus));
    EXPECT_FLOAT_EQ(0.8f, wetEngine.GetBusReverbSettings(wetAmbientBus).wetLevel);

    std::vector<float> wetFrames(static_cast<size_t>(frameCount * 2u), 0.0f);
    ASSERT_EQ(frameCount, wetEngine.ReadMixedFrames(wetFrames.data(), frameCount));
    const float wetTailRms = CalculateRmsRange(wetFrames, 1200u * 2u, wetFrames.size());
    EXPECT_GT(wetTailRms, 0.0005f);
    EXPECT_GT(wetTailRms, dryTailRms * 10.0f);

    wetEngine.Shutdown();
    dryEngine.Shutdown();
    RemoveTempFile(wavPath);
}

TEST(AudioSpatialValidation, AudioEventGeneratedSettingsIncludeTargetBus)
{
    RVX::Audio::AudioEventDesc desc;
    desc.targetBus = RVX::Audio::BusId::Voice;
    desc.volumeMin = 0.5f;
    desc.volumeMax = 0.5f;
    desc.pitchMin = 1.25f;
    desc.pitchMax = 1.25f;
    desc.loop = true;
    desc.fadeInTime = 0.125f;

    RVX::Audio::AudioEvent event(desc);
    const RVX::Audio::AudioPlaySettings settings = event.GenerateSettings();

    EXPECT_EQ(RVX::Audio::BusId::Voice, settings.busId);
    EXPECT_FLOAT_EQ(0.5f, settings.volume);
    EXPECT_FLOAT_EQ(1.25f, settings.pitch);
    EXPECT_TRUE(settings.loop);
    EXPECT_FLOAT_EQ(0.125f, settings.fadeInTime);
}

TEST(AudioSpatialValidation, AudioSubsystemRoutesQuickPlayThroughDefaultAndRequestedBuses)
{
    LogScope logScope;
    const std::filesystem::path firstPath = WriteSilentWav("rvx_audio_subsystem_bus_first.wav");
    const std::filesystem::path secondPath = WriteSilentWav("rvx_audio_subsystem_bus_second.wav");
    ASSERT_FALSE(firstPath.empty());
    ASSERT_FALSE(secondPath.empty());

    RVX::Audio::AudioEngineConfig config;
    config.enableDevice = false;
    config.resourceManagerJobThreadCount = 0;

    RVX::Audio::AudioSubsystem subsystem;
    subsystem.SetConfig(config);
    subsystem.Initialize();
    ASSERT_TRUE(subsystem.GetEngine().IsInitialized());
    ASSERT_NE(nullptr, subsystem.GetMixer());

    EXPECT_EQ(6u, subsystem.GetEngine().GetBusCount());
    EXPECT_EQ(RVX::Audio::BusId::Master, subsystem.GetMasterBusId());
    EXPECT_EQ(RVX::Audio::BusId::Music, subsystem.GetBusId("Music"));
    EXPECT_EQ(RVX::Audio::BusId::SFX, subsystem.GetBusId("SFX"));
    EXPECT_EQ(RVX::Audio::BusId::Ambient, subsystem.GetBusId("Ambient"));
    EXPECT_EQ(RVX::RVX_INVALID_INDEX, subsystem.GetBusId("Missing"));

    const RVX::Audio::AudioHandle defaultHandle = subsystem.PlaySound(firstPath.string());
    ASSERT_TRUE(defaultHandle.IsValid());
    EXPECT_EQ(subsystem.GetSFXBusId(), subsystem.GetEngine().GetSoundBus(defaultHandle));

    const RVX::Vec3 ambientPosition(3.0f, 4.0f, 5.0f);
    const RVX::Audio::AudioHandle ambientHandle =
        subsystem.PlaySound3D(secondPath.string(), ambientPosition, 1.0f, subsystem.GetAmbientBusId());
    ASSERT_TRUE(ambientHandle.IsValid());
    EXPECT_EQ(subsystem.GetAmbientBusId(), subsystem.GetEngine().GetSoundBus(ambientHandle));
    EXPECT_EQ(ambientPosition, subsystem.GetEngine().GetSoundPosition(ambientHandle));

    const RVX::Audio::AudioHandle masterHandle =
        subsystem.PlaySound(firstPath.string(), 0.5f, subsystem.GetMasterBusId());
    ASSERT_TRUE(masterHandle.IsValid());
    EXPECT_EQ(subsystem.GetMasterBusId(), subsystem.GetEngine().GetSoundBus(masterHandle));

    subsystem.Deinitialize();
    RemoveTempFile(firstPath);
    RemoveTempFile(secondPath);
}

TEST(AudioSpatialValidation, AudioSubsystemMixerLowPassEffectSyncsToRuntimeBus)
{
    LogScope logScope;
    const std::filesystem::path wavPath =
        WriteSineWav("rvx_audio_subsystem_mixer_low_pass.wav", 8000u, 3000.0f, 2048u);
    ASSERT_FALSE(wavPath.empty());

    RVX::Audio::AudioEngineConfig config;
    config.enableDevice = false;
    config.sampleRate = 8000;
    config.channels = 1;
    config.bufferSizeFrames = 128;
    config.resourceManagerJobThreadCount = 0;

    RVX::Audio::AudioEngine dryEngine;
    ASSERT_TRUE(dryEngine.Initialize(config));
    const RVX::uint32 dryAmbientBus = dryEngine.CreateBus("Ambient", RVX::Audio::BusId::Master);
    ASSERT_EQ(RVX::Audio::BusId::Ambient, dryAmbientBus);
    RVX::Audio::AudioClip::Ptr dryClip = dryEngine.LoadClip(wavPath.string());
    ASSERT_TRUE(dryClip);

    RVX::Audio::AudioPlaySettings drySettings;
    drySettings.busId = dryAmbientBus;
    ASSERT_TRUE(dryEngine.Play(dryClip, drySettings).IsValid());

    std::vector<float> dryFrames(512u, 0.0f);
    const RVX::uint64 dryRead = dryEngine.ReadMixedFrames(dryFrames.data(), static_cast<RVX::uint64>(dryFrames.size()));
    ASSERT_EQ(static_cast<RVX::uint64>(dryFrames.size()), dryRead);
    const float dryRms = CalculateRms(dryFrames, dryRead);
    EXPECT_GT(dryRms, 0.1f);

    RVX::Audio::AudioSubsystem subsystem;
    subsystem.SetConfig(config);
    subsystem.Initialize();
    ASSERT_TRUE(subsystem.GetEngine().IsInitialized());
    ASSERT_NE(nullptr, subsystem.GetMixer());

    RVX::Audio::AudioClip::Ptr filteredClip = subsystem.GetEngine().LoadClip(wavPath.string());
    ASSERT_TRUE(filteredClip);

    RVX::Audio::AudioPlaySettings filteredSettings;
    filteredSettings.busId = RVX::Audio::BusId::Ambient;
    ASSERT_TRUE(subsystem.GetEngine().Play(filteredClip, filteredSettings).IsValid());

    auto lowPass = std::make_shared<RVX::Audio::LowPassEffect>();
    lowPass->SetParameter("cutoff", 500.0f);
    ASSERT_TRUE(subsystem.AddBusEffect(RVX::Audio::BusId::Ambient, lowPass));

    const RVX::Audio::AudioBusNode* ambientBus = subsystem.GetMixer()->GetBus(RVX::Audio::BusId::Ambient);
    ASSERT_NE(nullptr, ambientBus);
    EXPECT_EQ(1u, ambientBus->GetEffects().size());
    EXPECT_NEAR(500.0f, subsystem.GetEngine().GetBusLowPassCutoff(RVX::Audio::BusId::Ambient), 0.0001f);

    std::vector<float> filteredFrames(512u, 0.0f);
    const RVX::uint64 filteredRead =
        subsystem.GetEngine().ReadMixedFrames(filteredFrames.data(), static_cast<RVX::uint64>(filteredFrames.size()));
    ASSERT_EQ(static_cast<RVX::uint64>(filteredFrames.size()), filteredRead);
    const float filteredRms = CalculateRms(filteredFrames, filteredRead);
    EXPECT_LT(filteredRms, dryRms * 0.65f);

    lowPass->SetEnabled(false);
    subsystem.SyncBusEffects(RVX::Audio::BusId::Ambient);
    EXPECT_NEAR(20000.0f, subsystem.GetEngine().GetBusLowPassCutoff(RVX::Audio::BusId::Ambient), 0.0001f);

    subsystem.Deinitialize();
    dryEngine.Shutdown();
    RemoveTempFile(wavPath);
}

TEST(AudioSpatialValidation, AudioSubsystemTickAutoSyncsMixerBusEffectMutations)
{
    LogScope logScope;
    const std::filesystem::path wavPath =
        WriteSineWav("rvx_audio_subsystem_mixer_auto_sync_low_pass.wav", 8000u, 3000.0f, 2048u);
    ASSERT_FALSE(wavPath.empty());

    RVX::Audio::AudioEngineConfig config;
    config.enableDevice = false;
    config.sampleRate = 8000;
    config.channels = 1;
    config.bufferSizeFrames = 128;
    config.resourceManagerJobThreadCount = 0;

    RVX::Audio::AudioEngine dryEngine;
    ASSERT_TRUE(dryEngine.Initialize(config));
    const RVX::uint32 dryAmbientBus = dryEngine.CreateBus("Ambient", RVX::Audio::BusId::Master);
    ASSERT_EQ(RVX::Audio::BusId::Ambient, dryAmbientBus);
    RVX::Audio::AudioClip::Ptr dryClip = dryEngine.LoadClip(wavPath.string());
    ASSERT_TRUE(dryClip);

    RVX::Audio::AudioPlaySettings drySettings;
    drySettings.busId = dryAmbientBus;
    ASSERT_TRUE(dryEngine.Play(dryClip, drySettings).IsValid());

    std::vector<float> dryFrames(512u, 0.0f);
    const RVX::uint64 dryRead = dryEngine.ReadMixedFrames(dryFrames.data(), static_cast<RVX::uint64>(dryFrames.size()));
    ASSERT_EQ(static_cast<RVX::uint64>(dryFrames.size()), dryRead);
    const float dryRms = CalculateRms(dryFrames, dryRead);
    EXPECT_GT(dryRms, 0.1f);

    RVX::Audio::AudioSubsystem subsystem;
    subsystem.SetConfig(config);
    subsystem.Initialize();
    ASSERT_TRUE(subsystem.GetEngine().IsInitialized());
    ASSERT_NE(nullptr, subsystem.GetMixer());

    RVX::Audio::AudioBusNode* ambientBus = subsystem.GetMixer()->GetBus(RVX::Audio::BusId::Ambient);
    ASSERT_NE(nullptr, ambientBus);

    RVX::Audio::AudioClip::Ptr filteredClip = subsystem.GetEngine().LoadClip(wavPath.string());
    ASSERT_TRUE(filteredClip);

    RVX::Audio::AudioPlaySettings filteredSettings;
    filteredSettings.busId = RVX::Audio::BusId::Ambient;
    ASSERT_TRUE(subsystem.GetEngine().Play(filteredClip, filteredSettings).IsValid());

    auto lowPass = std::make_shared<RVX::Audio::LowPassEffect>();
    lowPass->SetParameter("cutoff", 2000.0f);
    ambientBus->AddEffect(lowPass);

    subsystem.Tick(0.0f);
    EXPECT_NEAR(2000.0f, subsystem.GetEngine().GetBusLowPassCutoff(RVX::Audio::BusId::Ambient), 0.0001f);

    lowPass->SetParameter("cutoff", 500.0f);
    subsystem.Tick(0.0f);
    EXPECT_NEAR(500.0f, subsystem.GetEngine().GetBusLowPassCutoff(RVX::Audio::BusId::Ambient), 0.0001f);

    std::vector<float> filteredFrames(512u, 0.0f);
    const RVX::uint64 filteredRead =
        subsystem.GetEngine().ReadMixedFrames(filteredFrames.data(), static_cast<RVX::uint64>(filteredFrames.size()));
    ASSERT_EQ(static_cast<RVX::uint64>(filteredFrames.size()), filteredRead);
    const float filteredRms = CalculateRms(filteredFrames, filteredRead);
    EXPECT_LT(filteredRms, dryRms * 0.65f);

    lowPass->SetEnabled(false);
    subsystem.Tick(0.0f);
    EXPECT_NEAR(20000.0f, subsystem.GetEngine().GetBusLowPassCutoff(RVX::Audio::BusId::Ambient), 0.0001f);

    subsystem.Deinitialize();
    dryEngine.Shutdown();
    RemoveTempFile(wavPath);
}

TEST(AudioSpatialValidation, AudioSubsystemMixerHighPassEffectSyncsThroughGenericRuntimeNode)
{
    LogScope logScope;
    const std::filesystem::path wavPath =
        WriteSineWav("rvx_audio_subsystem_mixer_high_pass.wav", 44100u, 120.0f, 8192u);
    ASSERT_FALSE(wavPath.empty());

    RVX::Audio::AudioEngineConfig config;
    config.enableDevice = false;
    config.sampleRate = 44100;
    config.channels = 1;
    config.bufferSizeFrames = 256;
    config.resourceManagerJobThreadCount = 0;

    RVX::Audio::AudioEngine dryEngine;
    ASSERT_TRUE(dryEngine.Initialize(config));
    const RVX::uint32 dryAmbientBus = dryEngine.CreateBus("Ambient", RVX::Audio::BusId::Master);
    ASSERT_EQ(RVX::Audio::BusId::Ambient, dryAmbientBus);
    RVX::Audio::AudioClip::Ptr dryClip = dryEngine.LoadClip(wavPath.string());
    ASSERT_TRUE(dryClip);

    RVX::Audio::AudioPlaySettings drySettings;
    drySettings.busId = dryAmbientBus;
    ASSERT_TRUE(dryEngine.Play(dryClip, drySettings).IsValid());

    std::vector<float> dryFrames(2048u, 0.0f);
    const RVX::uint64 dryRead = dryEngine.ReadMixedFrames(dryFrames.data(), static_cast<RVX::uint64>(dryFrames.size()));
    ASSERT_EQ(static_cast<RVX::uint64>(dryFrames.size()), dryRead);
    const float dryRms = CalculateRms(dryFrames, dryRead);
    EXPECT_GT(dryRms, 0.1f);

    RVX::Audio::AudioSubsystem subsystem;
    subsystem.SetConfig(config);
    subsystem.Initialize();
    ASSERT_TRUE(subsystem.GetEngine().IsInitialized());
    ASSERT_NE(nullptr, subsystem.GetMixer());

    RVX::Audio::AudioClip::Ptr filteredClip = subsystem.GetEngine().LoadClip(wavPath.string());
    ASSERT_TRUE(filteredClip);

    RVX::Audio::AudioPlaySettings filteredSettings;
    filteredSettings.busId = RVX::Audio::BusId::Ambient;
    ASSERT_TRUE(subsystem.GetEngine().Play(filteredClip, filteredSettings).IsValid());

    auto highPass = std::make_shared<RVX::Audio::HighPassEffect>();
    highPass->SetParameter("cutoff", 2000.0f);
    ASSERT_TRUE(subsystem.AddBusEffect(RVX::Audio::BusId::Ambient, highPass));

    std::vector<float> filteredFrames(2048u, 0.0f);
    const RVX::uint64 filteredRead =
        subsystem.GetEngine().ReadMixedFrames(filteredFrames.data(), static_cast<RVX::uint64>(filteredFrames.size()));
    ASSERT_EQ(static_cast<RVX::uint64>(filteredFrames.size()), filteredRead);
    const float filteredRms = CalculateRms(filteredFrames, filteredRead);
    EXPECT_LT(filteredRms, dryRms * 0.35f);

    subsystem.Deinitialize();
    dryEngine.Shutdown();
    RemoveTempFile(wavPath);
}

TEST(AudioSpatialValidation, AudioSubsystemMixerReverbEffectSyncsToRuntimeBus)
{
    LogScope logScope;
    constexpr RVX::uint32 sampleRate = 44100;
    constexpr RVX::uint64 frameCount = 4096;
    const std::filesystem::path wavPath =
        WriteStereoImpulseWav("rvx_audio_subsystem_mixer_reverb.wav", sampleRate, static_cast<std::uint32_t>(frameCount));
    ASSERT_FALSE(wavPath.empty());

    RVX::Audio::AudioEngineConfig config;
    config.enableDevice = false;
    config.sampleRate = sampleRate;
    config.channels = 2;
    config.bufferSizeFrames = 256;
    config.resourceManagerJobThreadCount = 0;

    RVX::Audio::AudioEngine dryEngine;
    ASSERT_TRUE(dryEngine.Initialize(config));
    const RVX::uint32 dryAmbientBus = dryEngine.CreateBus("Ambient", RVX::Audio::BusId::Master);
    ASSERT_EQ(RVX::Audio::BusId::Ambient, dryAmbientBus);
    RVX::Audio::AudioClip::Ptr dryClip = dryEngine.LoadClip(wavPath.string());
    ASSERT_TRUE(dryClip);

    RVX::Audio::AudioPlaySettings drySettings;
    drySettings.busId = dryAmbientBus;
    ASSERT_TRUE(dryEngine.Play(dryClip, drySettings).IsValid());

    std::vector<float> dryFrames(static_cast<size_t>(frameCount * 2u), 0.0f);
    ASSERT_EQ(frameCount, dryEngine.ReadMixedFrames(dryFrames.data(), frameCount));
    const float dryTailRms = CalculateRmsRange(dryFrames, 1200u * 2u, dryFrames.size());
    EXPECT_LT(dryTailRms, 0.0001f);

    RVX::Audio::AudioSubsystem subsystem;
    subsystem.SetConfig(config);
    subsystem.Initialize();
    ASSERT_TRUE(subsystem.GetEngine().IsInitialized());
    ASSERT_NE(nullptr, subsystem.GetMixer());

    RVX::Audio::AudioClip::Ptr wetClip = subsystem.GetEngine().LoadClip(wavPath.string());
    ASSERT_TRUE(wetClip);

    RVX::Audio::AudioPlaySettings wetSettings;
    wetSettings.busId = RVX::Audio::BusId::Ambient;
    ASSERT_TRUE(subsystem.GetEngine().Play(wetClip, wetSettings).IsValid());

    auto reverb = std::make_shared<RVX::Audio::ReverbEffect>();
    reverb->SetParameter("roomSize", 0.8f);
    reverb->SetParameter("damping", 0.3f);
    reverb->SetParameter("wetLevel", 0.8f);
    reverb->SetParameter("dryLevel", 0.2f);
    reverb->SetParameter("width", 1.0f);
    ASSERT_TRUE(subsystem.AddBusEffect(RVX::Audio::BusId::Ambient, reverb));

    EXPECT_TRUE(subsystem.GetEngine().IsBusReverbEnabled(RVX::Audio::BusId::Ambient));
    EXPECT_FLOAT_EQ(0.8f, subsystem.GetEngine().GetBusReverbSettings(RVX::Audio::BusId::Ambient).wetLevel);

    std::vector<float> wetFrames(static_cast<size_t>(frameCount * 2u), 0.0f);
    ASSERT_EQ(frameCount, subsystem.GetEngine().ReadMixedFrames(wetFrames.data(), frameCount));
    const float wetTailRms = CalculateRmsRange(wetFrames, 1200u * 2u, wetFrames.size());
    EXPECT_GT(wetTailRms, 0.0005f);
    EXPECT_GT(wetTailRms, dryTailRms * 10.0f);

    reverb->SetEnabled(false);
    subsystem.SyncBusEffects(RVX::Audio::BusId::Ambient);
    EXPECT_FALSE(subsystem.GetEngine().IsBusReverbEnabled(RVX::Audio::BusId::Ambient));

    subsystem.Deinitialize();
    dryEngine.Shutdown();
    RemoveTempFile(wavPath);
}

TEST(AudioSpatialValidation, AudioSubsystemMixerBusSendRoutesThroughReturnBusEffects)
{
    LogScope logScope;
    constexpr RVX::uint32 sampleRate = 44100;
    constexpr RVX::uint64 frameCount = 4096;
    const std::filesystem::path wavPath =
        WriteStereoImpulseWav("rvx_audio_subsystem_mixer_send_return.wav", sampleRate, static_cast<std::uint32_t>(frameCount));
    ASSERT_FALSE(wavPath.empty());

    RVX::Audio::AudioEngineConfig config;
    config.enableDevice = false;
    config.sampleRate = sampleRate;
    config.channels = 2;
    config.bufferSizeFrames = 256;
    config.resourceManagerJobThreadCount = 0;

    RVX::Audio::AudioEngine dryEngine;
    ASSERT_TRUE(dryEngine.Initialize(config));
    const RVX::uint32 dryAmbientBus = dryEngine.CreateBus("Ambient", RVX::Audio::BusId::Master);
    ASSERT_EQ(RVX::Audio::BusId::Ambient, dryAmbientBus);
    RVX::Audio::AudioClip::Ptr dryClip = dryEngine.LoadClip(wavPath.string());
    ASSERT_TRUE(dryClip);

    RVX::Audio::AudioPlaySettings drySettings;
    drySettings.busId = dryAmbientBus;
    ASSERT_TRUE(dryEngine.Play(dryClip, drySettings).IsValid());

    std::vector<float> dryFrames(static_cast<size_t>(frameCount * 2u), 0.0f);
    ASSERT_EQ(frameCount, dryEngine.ReadMixedFrames(dryFrames.data(), frameCount));
    const float dryTailRms = CalculateRmsRange(dryFrames, 1200u * 2u, dryFrames.size());
    EXPECT_LT(dryTailRms, 0.0001f);

    RVX::Audio::AudioSubsystem subsystem;
    subsystem.SetConfig(config);
    subsystem.Initialize();
    ASSERT_TRUE(subsystem.GetEngine().IsInitialized());
    ASSERT_NE(nullptr, subsystem.GetMixer());

    auto returnReverb = std::make_shared<RVX::Audio::ReverbEffect>();
    returnReverb->SetParameter("roomSize", 0.8f);
    returnReverb->SetParameter("damping", 0.3f);
    returnReverb->SetParameter("wetLevel", 0.8f);
    returnReverb->SetParameter("dryLevel", 0.0f);
    returnReverb->SetParameter("width", 1.0f);
    ASSERT_TRUE(subsystem.AddBusEffect(RVX::Audio::BusId::UI, returnReverb));
    ASSERT_TRUE(subsystem.SetBusSend(RVX::Audio::BusId::Ambient, RVX::Audio::BusId::UI, 1.0f));

    RVX::Audio::AudioClip::Ptr wetClip = subsystem.GetEngine().LoadClip(wavPath.string());
    ASSERT_TRUE(wetClip);

    RVX::Audio::AudioPlaySettings wetSettings;
    wetSettings.busId = RVX::Audio::BusId::Ambient;
    ASSERT_TRUE(subsystem.GetEngine().Play(wetClip, wetSettings).IsValid());

    std::vector<float> wetFrames(static_cast<size_t>(frameCount * 2u), 0.0f);
    ASSERT_EQ(frameCount, subsystem.GetEngine().ReadMixedFrames(wetFrames.data(), frameCount));
    const float wetTailRms = CalculateRmsRange(wetFrames, 1200u * 2u, wetFrames.size());
    EXPECT_GT(wetTailRms, 0.0005f);
    EXPECT_GT(wetTailRms, dryTailRms * 10.0f);

    subsystem.Deinitialize();
    dryEngine.Shutdown();
    RemoveTempFile(wavPath);
}

TEST(AudioSpatialValidation, AudioSubsystemRejectsCyclicMixerBusSends)
{
    LogScope logScope;

    RVX::Audio::AudioEngineConfig config;
    config.enableDevice = false;
    config.resourceManagerJobThreadCount = 0;

    RVX::Audio::AudioSubsystem subsystem;
    subsystem.SetConfig(config);
    subsystem.Initialize();
    ASSERT_TRUE(subsystem.GetEngine().IsInitialized());
    ASSERT_NE(nullptr, subsystem.GetMixer());

    ASSERT_TRUE(subsystem.SetBusSend(RVX::Audio::BusId::Ambient, RVX::Audio::BusId::UI, 1.0f));
    EXPECT_FALSE(subsystem.SetBusSend(RVX::Audio::BusId::UI, RVX::Audio::BusId::Ambient, 1.0f));

    const RVX::Audio::AudioBusNode* ambientBus = subsystem.GetMixer()->GetBus(RVX::Audio::BusId::Ambient);
    const RVX::Audio::AudioBusNode* uiBus = subsystem.GetMixer()->GetBus(RVX::Audio::BusId::UI);
    ASSERT_NE(nullptr, ambientBus);
    ASSERT_NE(nullptr, uiBus);
    EXPECT_FLOAT_EQ(1.0f, ambientBus->GetSend(RVX::Audio::BusId::UI));
    EXPECT_FLOAT_EQ(0.0f, uiBus->GetSend(RVX::Audio::BusId::Ambient));

    const RVX::Audio::AudioEngine::Statistics stats = subsystem.GetEngine().GetStatistics();
    EXPECT_EQ(1u, stats.activeBusSends);
    EXPECT_EQ(0u, stats.droppedBusSends);

    subsystem.Deinitialize();
}

TEST(AudioSpatialValidation, AudioEngineReportsDroppedBusSendsForCyclesAndOverflow)
{
    LogScope logScope;

    RVX::Audio::AudioEngineConfig config;
    config.enableDevice = false;
    config.resourceManagerJobThreadCount = 0;

    RVX::Audio::AudioEngine engine;
    ASSERT_TRUE(engine.Initialize(config));
    const RVX::uint32 musicBus = engine.CreateBus("Music", RVX::Audio::BusId::Master);
    const RVX::uint32 sfxBus = engine.CreateBus("SFX", RVX::Audio::BusId::Master);
    const RVX::uint32 voiceBus = engine.CreateBus("Voice", RVX::Audio::BusId::Master);
    const RVX::uint32 ambientBus = engine.CreateBus("Ambient", RVX::Audio::BusId::Master);
    const RVX::uint32 uiBus = engine.CreateBus("UI", RVX::Audio::BusId::Master);
    ASSERT_EQ(RVX::Audio::BusId::Music, musicBus);
    ASSERT_EQ(RVX::Audio::BusId::SFX, sfxBus);
    ASSERT_EQ(RVX::Audio::BusId::Voice, voiceBus);
    ASSERT_EQ(RVX::Audio::BusId::Ambient, ambientBus);
    ASSERT_EQ(RVX::Audio::BusId::UI, uiBus);

    engine.SetBusSends(ambientBus, {{uiBus, 1.0f}});
    RVX::Audio::AudioEngine::Statistics stats = engine.GetStatistics();
    EXPECT_EQ(1u, stats.activeBusSends);
    EXPECT_EQ(0u, stats.droppedBusSends);

    engine.SetBusSends(uiBus, {{ambientBus, 1.0f}});
    stats = engine.GetStatistics();
    EXPECT_EQ(1u, stats.activeBusSends);
    EXPECT_EQ(1u, stats.droppedBusSends);

    engine.SetBusSends(ambientBus, {
        {musicBus, 1.0f},
        {sfxBus, 1.0f},
        {voiceBus, 1.0f},
        {uiBus, 1.0f},
        {RVX::Audio::BusId::Master, 1.0f}
    });
    stats = engine.GetStatistics();
    EXPECT_EQ(4u, stats.activeBusSends);
    EXPECT_GE(stats.droppedBusSends, 2u);

    engine.Shutdown();
}

TEST(AudioSpatialValidation, AudioZoneLowPassFiltersAmbientBusMixedOutput)
{
    LogScope logScope;
    const std::filesystem::path wavPath =
        WriteSineWav("rvx_audio_zone_ambient_low_pass_filter.wav", 8000u, 3000.0f, 2048u);
    ASSERT_FALSE(wavPath.empty());

    RVX::Audio::AudioEngineConfig config;
    config.enableDevice = false;
    config.sampleRate = 8000;
    config.channels = 1;
    config.bufferSizeFrames = 128;
    config.resourceManagerJobThreadCount = 0;

    RVX::Audio::AudioEngine dryEngine;
    ASSERT_TRUE(dryEngine.Initialize(config));
    const RVX::uint32 dryAmbientBus = dryEngine.CreateBus("Ambient", RVX::Audio::BusId::Master);
    ASSERT_EQ(RVX::Audio::BusId::Ambient, dryAmbientBus);
    RVX::Audio::AudioClip::Ptr dryClip = dryEngine.LoadClip(wavPath.string());
    ASSERT_TRUE(dryClip);

    RVX::Audio::AudioPlaySettings drySettings;
    drySettings.busId = dryAmbientBus;
    ASSERT_TRUE(dryEngine.Play(dryClip, drySettings).IsValid());

    std::vector<float> dryFrames(512u, 0.0f);
    const RVX::uint64 dryRead = dryEngine.ReadMixedFrames(dryFrames.data(), static_cast<RVX::uint64>(dryFrames.size()));
    ASSERT_EQ(static_cast<RVX::uint64>(dryFrames.size()), dryRead);
    const float dryRms = CalculateRms(dryFrames, dryRead);
    EXPECT_GT(dryRms, 0.1f);

    RVX::Audio::AudioSubsystem subsystem;
    subsystem.SetConfig(config);
    subsystem.Initialize();
    ASSERT_TRUE(subsystem.GetEngine().IsInitialized());
    ASSERT_NE(nullptr, subsystem.GetZoneManager());

    RVX::Audio::AudioClip::Ptr zoneClip = subsystem.GetEngine().LoadClip(wavPath.string());
    ASSERT_TRUE(zoneClip);

    RVX::Audio::AudioZone::Ptr zone = RVX::Audio::AudioZone::CreateBox(
        "UnderwaterAmbient",
        RVX::Vec3(0.0f),
        RVX::Vec3(10.0f));
    zone->SetAmbientClip(zoneClip);
    zone->SetAmbientVolume(1.0f);
    zone->SetLowPassEnabled(true);
    zone->SetLowPassCutoff(500.0f);
    subsystem.GetZoneManager()->AddZone(zone);

    subsystem.SetListenerTransform(RVX::Vec3(0.0f));
    subsystem.Tick(1.0f);
    EXPECT_NEAR(500.0f, subsystem.GetEngine().GetBusLowPassCutoff(RVX::Audio::BusId::Ambient), 0.0001f);

    std::vector<float> filteredFrames(512u, 0.0f);
    const RVX::uint64 filteredRead =
        subsystem.GetEngine().ReadMixedFrames(filteredFrames.data(), static_cast<RVX::uint64>(filteredFrames.size()));
    ASSERT_EQ(static_cast<RVX::uint64>(filteredFrames.size()), filteredRead);
    const float filteredRms = CalculateRms(filteredFrames, filteredRead);
    EXPECT_LT(filteredRms, dryRms * 0.65f);

    subsystem.Deinitialize();
    dryEngine.Shutdown();
    RemoveTempFile(wavPath);
}

TEST(AudioSpatialValidation, AudioZoneReverbAddsAmbientBusTailToMixedOutput)
{
    LogScope logScope;
    constexpr RVX::uint32 sampleRate = 44100;
    constexpr RVX::uint64 frameCount = 4096;
    const std::filesystem::path wavPath =
        WriteStereoImpulseWav("rvx_audio_zone_ambient_reverb.wav", sampleRate, static_cast<std::uint32_t>(frameCount));
    ASSERT_FALSE(wavPath.empty());

    RVX::Audio::AudioEngineConfig config;
    config.enableDevice = false;
    config.sampleRate = sampleRate;
    config.channels = 2;
    config.bufferSizeFrames = 256;
    config.resourceManagerJobThreadCount = 0;

    RVX::Audio::AudioEngine dryEngine;
    ASSERT_TRUE(dryEngine.Initialize(config));
    const RVX::uint32 dryAmbientBus = dryEngine.CreateBus("Ambient", RVX::Audio::BusId::Master);
    ASSERT_EQ(RVX::Audio::BusId::Ambient, dryAmbientBus);
    RVX::Audio::AudioClip::Ptr dryClip = dryEngine.LoadClip(wavPath.string());
    ASSERT_TRUE(dryClip);

    RVX::Audio::AudioPlaySettings drySettings;
    drySettings.busId = dryAmbientBus;
    ASSERT_TRUE(dryEngine.Play(dryClip, drySettings).IsValid());

    std::vector<float> dryFrames(static_cast<size_t>(frameCount * 2u), 0.0f);
    ASSERT_EQ(frameCount, dryEngine.ReadMixedFrames(dryFrames.data(), frameCount));
    const float dryTailRms = CalculateRmsRange(dryFrames, 1200u * 2u, dryFrames.size());
    EXPECT_LT(dryTailRms, 0.0001f);

    RVX::Audio::AudioSubsystem subsystem;
    subsystem.SetConfig(config);
    subsystem.Initialize();
    ASSERT_TRUE(subsystem.GetEngine().IsInitialized());
    ASSERT_NE(nullptr, subsystem.GetZoneManager());

    RVX::Audio::AudioClip::Ptr zoneClip = subsystem.GetEngine().LoadClip(wavPath.string());
    ASSERT_TRUE(zoneClip);

    RVX::Audio::ReverbSettings reverb;
    reverb.roomSize = 0.8f;
    reverb.damping = 0.3f;
    reverb.wetLevel = 0.8f;
    reverb.dryLevel = 0.2f;
    reverb.width = 1.0f;

    RVX::Audio::AudioZone::Ptr zone = RVX::Audio::AudioZone::CreateBox(
        "CaveAmbient",
        RVX::Vec3(0.0f),
        RVX::Vec3(10.0f));
    zone->SetAmbientClip(zoneClip);
    zone->SetAmbientVolume(1.0f);
    zone->SetCustomReverb(reverb);
    subsystem.GetZoneManager()->AddZone(zone);

    subsystem.SetListenerTransform(RVX::Vec3(0.0f));
    subsystem.Tick(1.0f);
    EXPECT_TRUE(subsystem.GetEngine().IsBusReverbEnabled(RVX::Audio::BusId::Ambient));
    EXPECT_FLOAT_EQ(0.8f, subsystem.GetEngine().GetBusReverbSettings(RVX::Audio::BusId::Ambient).wetLevel);

    std::vector<float> wetFrames(static_cast<size_t>(frameCount * 2u), 0.0f);
    ASSERT_EQ(frameCount, subsystem.GetEngine().ReadMixedFrames(wetFrames.data(), frameCount));
    const float wetTailRms = CalculateRmsRange(wetFrames, 1200u * 2u, wetFrames.size());
    EXPECT_GT(wetTailRms, 0.0005f);
    EXPECT_GT(wetTailRms, dryTailRms * 10.0f);

    subsystem.Deinitialize();
    dryEngine.Shutdown();
    RemoveTempFile(wavPath);
}

TEST(AudioSpatialValidation, AudioSubsystemQuickPlayCacheEvictsLeastRecentlyUsedClip)
{
    LogScope logScope;
    const std::filesystem::path firstPath = WriteSilentWav("rvx_audio_lru_first.wav");
    const std::filesystem::path secondPath = WriteSilentWav("rvx_audio_lru_second.wav");
    ASSERT_FALSE(firstPath.empty());
    ASSERT_FALSE(secondPath.empty());

    RVX::Audio::AudioEngineConfig config;
    config.enableDevice = false;
    config.maxCachedClips = 1;
    config.resourceManagerJobThreadCount = 0;

    RVX::Audio::AudioSubsystem subsystem;
    subsystem.SetConfig(config);
    subsystem.Initialize();
    ASSERT_TRUE(subsystem.GetEngine().IsInitialized());
    EXPECT_EQ(1u, subsystem.GetMaxCachedClips());

    EXPECT_TRUE(subsystem.PlaySound(firstPath.string()).IsValid());
    EXPECT_EQ(1u, subsystem.GetCachedClipCount());
    EXPECT_TRUE(subsystem.HasCachedClip(firstPath.string()));

    EXPECT_TRUE(subsystem.PlaySound(secondPath.string()).IsValid());
    EXPECT_EQ(1u, subsystem.GetCachedClipCount());
    EXPECT_FALSE(subsystem.HasCachedClip(firstPath.string()));
    EXPECT_TRUE(subsystem.HasCachedClip(secondPath.string()));

    EXPECT_TRUE(subsystem.PlaySound(firstPath.string()).IsValid());
    EXPECT_EQ(1u, subsystem.GetCachedClipCount());
    EXPECT_TRUE(subsystem.HasCachedClip(firstPath.string()));
    EXPECT_FALSE(subsystem.HasCachedClip(secondPath.string()));

    subsystem.Deinitialize();
    RemoveTempFile(firstPath);
    RemoveTempFile(secondPath);
}

TEST(AudioSpatialValidation, AudioStreamerAppliesConfiguredPrefetchThreadPriority)
{
    LogScope logScope;
    const std::filesystem::path wavPath = WriteSilentWav("rvx_audio_streamer_priority.wav");
    ASSERT_FALSE(wavPath.empty());

    RVX::Audio::StreamingConfig config;
    config.enablePrefetch = true;
    config.bufferSize = 64;
    config.bufferCount = 2;
    config.prefetchThreshold = 2;
#if defined(_WIN32)
    config.prefetchThreadPriority = RVX::Audio::StreamingThreadPriority::High;
#else
    config.prefetchThreadPriority = RVX::Audio::StreamingThreadPriority::Normal;
#endif

    RVX::Audio::AudioStreamer streamer;
    streamer.SetConfig(config);
    ASSERT_TRUE(streamer.Open(wavPath.string()));
    EXPECT_EQ(config.prefetchThreadPriority, streamer.GetPrefetchThreadPriority());

    for (int i = 0; i < 50 && !streamer.WasPrefetchThreadPriorityApplied(); ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    EXPECT_TRUE(streamer.WasPrefetchThreadPriorityApplied());
    streamer.Close();
    EXPECT_FALSE(streamer.WasPrefetchThreadPriorityApplied());

    RemoveTempFile(wavPath);
}

TEST(AudioSpatialValidation, AudioSubsystemUsesPhysicsWorldOcclusionProvider)
{
    LogScope logScope;

    RVX::Physics::PhysicsWorldConfig physicsConfig;
    physicsConfig.gravity = RVX::Vec3(0.0f);

    RVX::Physics::PhysicsWorld physicsWorld;
    ASSERT_TRUE(physicsWorld.Initialize(physicsConfig));
    ASSERT_TRUE(CreateBoxBody(physicsWorld, RVX::Vec3(0.0f, 0.0f, 0.0f)).IsValid());

    RVX::Audio::AudioEngineConfig audioConfig;
    audioConfig.enableDevice = false;
    audioConfig.resourceManagerJobThreadCount = 0;

    RVX::Audio::AudioSubsystem subsystem;
    subsystem.SetConfig(audioConfig);
    subsystem.Initialize();
    ASSERT_TRUE(subsystem.GetEngine().IsInitialized());
    ASSERT_NE(nullptr, subsystem.GetZoneManager());

    subsystem.SetListenerTransform(RVX::Vec3(5.0f, 0.0f, 0.0f));
    subsystem.SetPhysicsWorld(&physicsWorld);

    const RVX::Audio::OcclusionResult blocked =
        subsystem.GetOcclusion(RVX::Vec3(-5.0f, 0.0f, 0.0f));
    EXPECT_NEAR(0.5f, blocked.occlusion, 0.0001f);
    EXPECT_NEAR(0.5f, blocked.obstruction, 0.0001f);
    EXPECT_NEAR(0.5f, blocked.transmission, 0.0001f);
    EXPECT_NEAR(18000.0f, blocked.lowPassCutoff, 0.0001f);
    EXPECT_NEAR(0.75f, blocked.volumeScale, 0.0001f);

    subsystem.SetPhysicsWorld(&physicsWorld, 1u << RVX::Physics::Layers::Trigger);
    const RVX::Audio::OcclusionResult filtered =
        subsystem.GetOcclusion(RVX::Vec3(-5.0f, 0.0f, 0.0f));
    EXPECT_FLOAT_EQ(0.0f, filtered.occlusion);
    EXPECT_FLOAT_EQ(1.0f, filtered.transmission);

    subsystem.SetPhysicsWorld(nullptr);
    const RVX::Audio::OcclusionResult disabled =
        subsystem.GetOcclusion(RVX::Vec3(-5.0f, 0.0f, 0.0f));
    EXPECT_FLOAT_EQ(0.0f, disabled.occlusion);

    subsystem.Deinitialize();
    physicsWorld.Shutdown();
}

TEST(AudioSpatialValidation, AudioComponentRoutesThroughBoundEngineAndUpdatesWorldPosition)
{
    LogScope logScope;
    const std::filesystem::path wavPath = WriteSilentWav("rvx_audio_component_bound_engine.wav");
    ASSERT_FALSE(wavPath.empty());

    RVX::Audio::AudioEngineConfig config;
    config.enableDevice = false;
    config.resourceManagerJobThreadCount = 0;

    RVX::Audio::AudioEngine engine;
    ASSERT_TRUE(engine.Initialize(config));
    const RVX::uint32 emitterBus = engine.CreateBus("Emitter", 0);

    RVX::Audio::AudioClip::Ptr clip = engine.LoadClip(wavPath.string());
    ASSERT_TRUE(clip);

    RVX::SceneEntity entity("BoundAudioEmitter");
    entity.SetPosition(RVX::Vec3(4.0f, 5.0f, 6.0f));

    auto* audio = entity.AddComponent<RVX::AudioComponent>();
    ASSERT_NE(nullptr, audio);
    audio->SetAudioEngine(&engine);
    audio->SetClip(clip);
    audio->SetBusId(emitterBus);
    audio->Play();

    const RVX::Audio::AudioHandle handle = audio->GetHandle();
    ASSERT_TRUE(handle.IsValid());
    EXPECT_EQ(emitterBus, engine.GetSoundBus(handle));
    EXPECT_EQ(RVX::Vec3(4.0f, 5.0f, 6.0f), engine.GetSoundPosition(handle));

    entity.SetPosition(RVX::Vec3(-2.0f, 3.0f, 9.0f));
    audio->Tick(1.0f / 60.0f);
    EXPECT_EQ(RVX::Vec3(-2.0f, 3.0f, 9.0f), engine.GetSoundPosition(handle));

    audio->Stop();
    engine.Shutdown();
    RemoveTempFile(wavPath);
}

TEST(AudioSpatialValidation, AudioComponentBoundSubsystemAppliesPhysicsOcclusionVolume)
{
    LogScope logScope;
    const std::filesystem::path wavPath = WriteSilentWav("rvx_audio_component_subsystem_occlusion.wav");
    ASSERT_FALSE(wavPath.empty());

    RVX::Physics::PhysicsWorldConfig physicsConfig;
    physicsConfig.gravity = RVX::Vec3(0.0f);

    RVX::Physics::PhysicsWorld physicsWorld;
    ASSERT_TRUE(physicsWorld.Initialize(physicsConfig));
    ASSERT_TRUE(CreateBoxBody(physicsWorld, RVX::Vec3(0.0f, 0.0f, 0.0f)).IsValid());

    RVX::Audio::AudioEngineConfig audioConfig;
    audioConfig.enableDevice = false;
    audioConfig.resourceManagerJobThreadCount = 0;

    RVX::Audio::AudioSubsystem subsystem;
    subsystem.SetConfig(audioConfig);
    subsystem.Initialize();
    ASSERT_TRUE(subsystem.GetEngine().IsInitialized());
    subsystem.SetListenerTransform(RVX::Vec3(5.0f, 0.0f, 0.0f));
    subsystem.SetPhysicsWorld(&physicsWorld);

    RVX::Audio::AudioClip::Ptr clip = subsystem.GetEngine().LoadClip(wavPath.string());
    ASSERT_TRUE(clip);

    RVX::SceneEntity entity("SubsystemAudioEmitter");
    entity.SetPosition(RVX::Vec3(-5.0f, 0.0f, 0.0f));

    auto* audio = entity.AddComponent<RVX::AudioComponent>();
    ASSERT_NE(nullptr, audio);
    audio->SetAudioSubsystem(&subsystem);
    audio->SetClip(clip);
    audio->SetVolume(0.8f);
    audio->Play();

    const RVX::Audio::AudioHandle handle = audio->GetHandle();
    ASSERT_TRUE(handle.IsValid());
    EXPECT_EQ(&subsystem, audio->GetBoundAudioSubsystem());
    EXPECT_NEAR(0.5f, audio->GetLastOcclusionResult().occlusion, 0.0001f);
    EXPECT_NEAR(18000.0f, audio->GetLastOcclusionResult().lowPassCutoff, 0.0001f);
    EXPECT_NEAR(0.6f, subsystem.GetEngine().GetSoundVolume(handle), 0.0001f);
    EXPECT_NEAR(18000.0f, subsystem.GetEngine().GetSoundLowPassCutoff(handle), 0.0001f);

    entity.SetPosition(RVX::Vec3(4.0f, 0.0f, 0.0f));
    audio->Tick(1.0f / 60.0f);
    EXPECT_FLOAT_EQ(0.0f, audio->GetLastOcclusionResult().occlusion);
    EXPECT_NEAR(0.8f, subsystem.GetEngine().GetSoundVolume(handle), 0.0001f);
    EXPECT_NEAR(20000.0f, subsystem.GetEngine().GetSoundLowPassCutoff(handle), 0.0001f);
    EXPECT_EQ(RVX::Vec3(4.0f, 0.0f, 0.0f), subsystem.GetEngine().GetSoundPosition(handle));

    audio->Stop();
    subsystem.Deinitialize();
    physicsWorld.Shutdown();
    RemoveTempFile(wavPath);
}

TEST(AudioSpatialValidation, AudioSubsystemIntegratedComponentOcclusionMixerSendSmoke)
{
    LogScope logScope;
    constexpr RVX::uint32 sampleRate = 44100;
    constexpr RVX::uint64 frameCount = 4096;
    const std::filesystem::path wavPath =
        WriteStereoImpulseWav("rvx_audio_integrated_component_mixer_smoke.wav", sampleRate, static_cast<std::uint32_t>(frameCount));
    ASSERT_FALSE(wavPath.empty());

    RVX::Physics::PhysicsWorldConfig physicsConfig;
    physicsConfig.gravity = RVX::Vec3(0.0f);

    RVX::Physics::PhysicsWorld physicsWorld;
    ASSERT_TRUE(physicsWorld.Initialize(physicsConfig));
    ASSERT_TRUE(CreateBoxBody(physicsWorld, RVX::Vec3(0.0f, 0.0f, 0.0f)).IsValid());

    RVX::Audio::AudioEngineConfig audioConfig;
    audioConfig.enableDevice = false;
    audioConfig.sampleRate = sampleRate;
    audioConfig.channels = 2;
    audioConfig.bufferSizeFrames = 256;
    audioConfig.maxCachedClips = 2;
    audioConfig.resourceManagerJobThreadCount = 0;

    RVX::Audio::AudioSubsystem subsystem;
    subsystem.SetConfig(audioConfig);
    subsystem.Initialize();
    ASSERT_TRUE(subsystem.GetEngine().IsInitialized());
    ASSERT_NE(nullptr, subsystem.GetMixer());
    ASSERT_NE(nullptr, subsystem.GetZoneManager());

    subsystem.SetListenerTransform(RVX::Vec3(5.0f, 0.0f, 0.0f));
    subsystem.SetPhysicsWorld(&physicsWorld);

    const RVX::Audio::AudioHandle cachedHandle = subsystem.PlaySound(wavPath.string(), 0.25f, subsystem.GetSFXBusId());
    ASSERT_TRUE(cachedHandle.IsValid());
    EXPECT_EQ(1u, subsystem.GetCachedClipCount());
    EXPECT_TRUE(subsystem.HasCachedClip(wavPath.string()));
    subsystem.GetEngine().Stop(cachedHandle);
    subsystem.Tick(0.0f);

    auto returnReverb = std::make_shared<RVX::Audio::ReverbEffect>();
    returnReverb->SetParameter("roomSize", 0.8f);
    returnReverb->SetParameter("damping", 0.3f);
    returnReverb->SetParameter("wetLevel", 0.8f);
    returnReverb->SetParameter("dryLevel", 0.0f);
    returnReverb->SetParameter("width", 1.0f);

    RVX::Audio::AudioBusNode* returnBus = subsystem.GetMixer()->GetBus(RVX::Audio::BusId::UI);
    RVX::Audio::AudioBusNode* ambientBus = subsystem.GetMixer()->GetBus(RVX::Audio::BusId::Ambient);
    ASSERT_NE(nullptr, returnBus);
    ASSERT_NE(nullptr, ambientBus);
    returnBus->AddEffect(returnReverb);
    ambientBus->SetSend(RVX::Audio::BusId::UI, 1.0f);
    subsystem.Tick(0.0f);

    EXPECT_TRUE(subsystem.GetEngine().IsBusReverbEnabled(RVX::Audio::BusId::UI));
    const RVX::Audio::AudioEngine::Statistics statsAfterSync = subsystem.GetEngine().GetStatistics();
    EXPECT_EQ(1u, statsAfterSync.activeBusSends);
    EXPECT_EQ(0u, statsAfterSync.droppedBusSends);

    RVX::Audio::AudioClip::Ptr clip = subsystem.GetEngine().LoadClip(wavPath.string());
    ASSERT_TRUE(clip);

    RVX::SceneEntity entity("IntegratedAudioEmitter");
    entity.SetPosition(RVX::Vec3(-5.0f, 0.0f, 0.0f));

    auto* audio = entity.AddComponent<RVX::AudioComponent>();
    ASSERT_NE(nullptr, audio);
    audio->SetAudioSubsystem(&subsystem);
    audio->SetClip(clip);

    RVX::AudioComponentSettings settings;
    settings.volume = 1.0f;
    settings.spatialize = true;
    settings.minDistance = 100.0f;
    settings.maxDistance = 200.0f;
    settings.attenuationModel = RVX::Audio::AttenuationModel::None;
    settings.busId = RVX::Audio::BusId::Ambient;
    audio->SetSettings(settings);
    audio->Play();

    const RVX::Audio::AudioHandle componentHandle = audio->GetHandle();
    ASSERT_TRUE(componentHandle.IsValid());
    EXPECT_EQ(RVX::Audio::BusId::Ambient, subsystem.GetEngine().GetSoundBus(componentHandle));
    EXPECT_NEAR(0.5f, audio->GetLastOcclusionResult().occlusion, 0.0001f);
    EXPECT_NEAR(0.75f, audio->GetLastOcclusionResult().volumeScale, 0.0001f);
    EXPECT_NEAR(0.75f, subsystem.GetEngine().GetSoundVolume(componentHandle), 0.0001f);
    EXPECT_NEAR(18000.0f, subsystem.GetEngine().GetSoundLowPassCutoff(componentHandle), 0.0001f);

    std::vector<float> mixedFrames(static_cast<size_t>(frameCount * 2u), 0.0f);
    ASSERT_EQ(frameCount, subsystem.GetEngine().ReadMixedFrames(mixedFrames.data(), frameCount));
    const float tailRms = CalculateRmsRange(mixedFrames, 1200u * 2u, mixedFrames.size());
    EXPECT_GT(tailRms, 0.0003f);

    audio->Stop();
    subsystem.Deinitialize();
    physicsWorld.Shutdown();
    RemoveTempFile(wavPath);
}
