#include "Resource/Loader/AudioLoader.h"
#include "Resource/ResourceCache.h"
#include "Core/Log.h"

// miniaudio configuration - enable all decoders
#define MA_NO_DEVICE_IO  // We just need decoding, not playback
#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio.h>

#include <filesystem>
#include <fstream>
#include <algorithm>
#include <functional>
#include <cmath>

namespace RVX::Resource
{
    // =========================================================================
    // Construction
    // =========================================================================

    AudioLoader::AudioLoader(ResourceManager* manager)
        : m_manager(manager)
    {
    }

    AudioLoader::~AudioLoader()
    {
        // m_silentAudio is managed by ResourceManager
    }

    // =========================================================================
    // IResourceLoader Interface
    // =========================================================================

    std::vector<std::string> AudioLoader::GetSupportedExtensions() const
    {
        return { ".wav", ".mp3", ".ogg", ".flac", ".aiff", ".aif" };
    }

    bool AudioLoader::CanLoad(const std::string& path) const
    {
        std::filesystem::path filePath(path);
        std::string ext = filePath.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

        auto extensions = GetSupportedExtensions();
        return std::find(extensions.begin(), extensions.end(), ext) != extensions.end();
    }

    IResource* AudioLoader::Load(const std::string& path)
    {
        AudioLoadOptions options;
        return LoadWithOptions(path, options);
    }

    // =========================================================================
    // Extended Loading API
    // =========================================================================

    AudioResource* AudioLoader::LoadWithOptions(const std::string& path,
                                                  const AudioLoadOptions& options)
    {
        // Resolve to absolute path
        std::filesystem::path absPath = std::filesystem::absolute(path);
        std::string absolutePath = absPath.string();

        ResourceId audioId = GenerateAudioId(absolutePath);

        // Check cache
        if (m_manager && m_manager->IsInitialized())
        {
            if (auto* cached = m_manager->GetCache().Get(audioId))
            {
                return static_cast<AudioResource*>(cached);
            }
        }

        // Check if file exists
        if (!std::filesystem::exists(absolutePath))
        {
            RVX_CORE_WARN("AudioLoader: File not found: {}", absolutePath);
            return GetSilentAudio();
        }

        // Check file size for auto-streaming
        AudioLoadOptions effectiveOptions = options;
        size_t fileSize = std::filesystem::file_size(absolutePath);
        if (!effectiveOptions.enableStreaming && fileSize > effectiveOptions.streamingThreshold)
        {
            RVX_CORE_INFO("AudioLoader: Large file ({}MB), enabling streaming for: {}", 
                fileSize / (1024 * 1024), absPath.filename().string());
            effectiveOptions.enableStreaming = true;
        }

        // For streaming, just create reference without loading data
        if (effectiveOptions.enableStreaming)
        {
            return LoadForStreaming(absolutePath);
        }

        // Decode audio
        std::vector<uint8_t> audioData;
        AudioMetadata metadata;

        if (!DecodeFile(absolutePath, audioData, metadata, effectiveOptions))
        {
            RVX_CORE_WARN("AudioLoader: Failed to decode: {}", absolutePath);
            return GetSilentAudio();
        }

        // Determine source format
        std::string ext = absPath.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (!ext.empty() && ext[0] == '.')
        {
            ext = ext.substr(1);
        }
        metadata.sourceFormat = ext;

        return CreateAudioResource(std::move(audioData), metadata, absolutePath);
    }

    AudioResource* AudioLoader::LoadFromMemory(const void* data, size_t size,
                                                 const std::string& uniqueKey,
                                                 const AudioLoadOptions& options)
    {
        ResourceId audioId = GenerateAudioId(uniqueKey);

        // Check cache
        if (m_manager && m_manager->IsInitialized())
        {
            if (auto* cached = m_manager->GetCache().Get(audioId))
            {
                return static_cast<AudioResource*>(cached);
            }
        }

        // Decode audio
        std::vector<uint8_t> audioData;
        AudioMetadata metadata;

        if (!DecodeMemory(data, size, audioData, metadata, options))
        {
            RVX_CORE_WARN("AudioLoader: Failed to decode from memory: {}", uniqueKey);
            return GetSilentAudio();
        }

        return CreateAudioResource(std::move(audioData), metadata, uniqueKey);
    }

    AudioResource* AudioLoader::LoadForStreaming(const std::string& path)
    {
        std::filesystem::path absPath = std::filesystem::absolute(path);
        std::string absolutePath = absPath.string();

        ResourceId audioId = GenerateAudioId(absolutePath + "_streaming");

        // Check cache
        if (m_manager && m_manager->IsInitialized())
        {
            if (auto* cached = m_manager->GetCache().Get(audioId))
            {
                return static_cast<AudioResource*>(cached);
            }
        }

        // Get file info without loading
        auto info = GetFileInfo(absolutePath);
        if (!info.valid)
        {
            RVX_CORE_WARN("AudioLoader: Cannot get info for streaming: {}", absolutePath);
            return GetSilentAudio();
        }

        // Create resource with metadata only
        auto* audio = new AudioResource();
        audio->SetId(audioId);
        audio->SetPath(absolutePath);
        audio->SetName(absPath.stem().string());

        AudioMetadata metadata;
        metadata.sampleRate = info.sampleRate;
        metadata.channels = info.channels;
        metadata.totalFrames = info.totalFrames;
        metadata.duration = info.duration;
        metadata.format = AudioFormat::S16;  // miniaudio typically decodes to S16
        metadata.bitsPerSample = 16;
        metadata.loadMode = AudioLoadMode::Streaming;
        metadata.sourceFormat = info.format;

        // Set empty data with metadata
        audio->SetData({}, metadata);
        audio->SetStreamingSourcePath(absolutePath);

        // Store in cache
        if (m_manager && m_manager->IsInitialized())
        {
            m_manager->GetCache().Store(audio);
        }

        RVX_CORE_INFO("AudioLoader: Prepared for streaming: {} ({:.1f}s)", 
            absPath.filename().string(), info.duration);

        return audio;
    }

    // =========================================================================
    // Default Audio
    // =========================================================================

    AudioResource* AudioLoader::GetSilentAudio()
    {
        if (!m_silentAudio)
        {
            // Create 0.1 second of silence
            uint32_t sampleRate = 44100;
            uint32_t channels = 2;
            uint32_t frames = sampleRate / 10;  // 0.1 second
            
            std::vector<uint8_t> silentData(frames * channels * 2, 0);  // S16 format

            AudioMetadata metadata;
            metadata.sampleRate = sampleRate;
            metadata.channels = channels;
            metadata.bitsPerSample = 16;
            metadata.totalFrames = frames;
            metadata.duration = 0.1f;
            metadata.format = AudioFormat::S16;
            metadata.sourceFormat = "generated";

            m_silentAudio = CreateAudioResource(std::move(silentData), metadata, "__silent_audio__");
        }
        return m_silentAudio;
    }

    AudioResource* AudioLoader::GenerateSineWave(float frequency, float duration,
                                                   uint32_t sampleRate)
    {
        std::string uniqueKey = "__sine_" + std::to_string(static_cast<int>(frequency)) + 
                                "_" + std::to_string(static_cast<int>(duration * 1000)) + "__";

        ResourceId audioId = GenerateAudioId(uniqueKey);

        // Check cache
        if (m_manager && m_manager->IsInitialized())
        {
            if (auto* cached = m_manager->GetCache().Get(audioId))
            {
                return static_cast<AudioResource*>(cached);
            }
        }

        uint32_t channels = 2;
        uint64_t totalFrames = static_cast<uint64_t>(duration * sampleRate);
        
        std::vector<uint8_t> data(totalFrames * channels * 2);  // S16 format
        int16_t* samples = reinterpret_cast<int16_t*>(data.data());

        const float twoPi = 2.0f * 3.14159265358979f;
        const float amplitude = 0.5f;  // 50% amplitude to avoid clipping

        for (uint64_t frame = 0; frame < totalFrames; ++frame)
        {
            float t = static_cast<float>(frame) / static_cast<float>(sampleRate);
            float sample = amplitude * std::sin(twoPi * frequency * t);
            
            // Apply fade in/out to avoid clicks
            float fadeTime = 0.01f;  // 10ms fade
            float frameFade = 1.0f;
            if (t < fadeTime)
            {
                frameFade = t / fadeTime;
            }
            else if (t > duration - fadeTime)
            {
                frameFade = (duration - t) / fadeTime;
            }
            sample *= frameFade;

            int16_t s16 = static_cast<int16_t>(sample * 32767.0f);
            samples[frame * 2] = s16;      // Left
            samples[frame * 2 + 1] = s16;  // Right
        }

        AudioMetadata metadata;
        metadata.sampleRate = sampleRate;
        metadata.channels = channels;
        metadata.bitsPerSample = 16;
        metadata.totalFrames = totalFrames;
        metadata.duration = duration;
        metadata.format = AudioFormat::S16;
        metadata.sourceFormat = "generated";

        return CreateAudioResource(std::move(data), metadata, uniqueKey);
    }

    // =========================================================================
    // Utilities
    // =========================================================================

    AudioLoader::AudioFileInfo AudioLoader::GetFileInfo(const std::string& path)
    {
        AudioFileInfo info;

        if (!std::filesystem::exists(path))
        {
            return info;
        }

        info.fileSize = std::filesystem::file_size(path);

        // Use miniaudio decoder to get info
        ma_decoder_config config = ma_decoder_config_init(ma_format_s16, 0, 0);
        ma_decoder decoder;
        
        ma_result result = ma_decoder_init_file(path.c_str(), &config, &decoder);
        if (result != MA_SUCCESS)
        {
            return info;
        }

        ma_uint64 totalFrames;
        result = ma_decoder_get_length_in_pcm_frames(&decoder, &totalFrames);
        if (result == MA_SUCCESS)
        {
            info.totalFrames = totalFrames;
        }

        info.sampleRate = decoder.outputSampleRate;
        info.channels = decoder.outputChannels;
        info.duration = static_cast<float>(info.totalFrames) / 
                        static_cast<float>(info.sampleRate);
        info.valid = true;

        // Determine format from extension
        std::filesystem::path filePath(path);
        std::string ext = filePath.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (!ext.empty() && ext[0] == '.')
        {
            ext = ext.substr(1);
        }
        info.format = ext;

        ma_decoder_uninit(&decoder);

        return info;
    }

    bool AudioLoader::IsFormatSupported(const std::string& extension)
    {
        std::string ext = extension;
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (!ext.empty() && ext[0] != '.')
        {
            ext = "." + ext;
        }

        static const std::vector<std::string> supported = {
            ".wav", ".mp3", ".ogg", ".flac", ".aiff", ".aif"
        };

        return std::find(supported.begin(), supported.end(), ext) != supported.end();
    }

    const char* AudioLoader::GetFormatDescription(const std::string& extension)
    {
        std::string ext = extension;
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

        if (ext == ".wav" || ext == "wav") return "Waveform Audio (PCM)";
        if (ext == ".mp3" || ext == "mp3") return "MPEG Audio Layer III";
        if (ext == ".ogg" || ext == "ogg") return "Ogg Vorbis";
        if (ext == ".flac" || ext == "flac") return "Free Lossless Audio Codec";
        if (ext == ".aiff" || ext == "aiff" || ext == ".aif" || ext == "aif")
            return "Audio Interchange File Format";

        return "Unknown Format";
    }

    // =========================================================================
    // Private Helpers
    // =========================================================================

    bool AudioLoader::DecodeFile(const std::string& path,
                                  std::vector<uint8_t>& outData,
                                  AudioMetadata& outMetadata,
                                  const AudioLoadOptions& options)
    {
        // Configure decoder
        ma_format outputFormat = ma_format_s16;
        ma_uint32 outputChannels = 0;  // 0 = use native
        ma_uint32 outputSampleRate = 0;  // 0 = use native

        if (options.targetFormat != AudioFormat::Unknown)
        {
            switch (options.targetFormat)
            {
                case AudioFormat::U8:  outputFormat = ma_format_u8; break;
                case AudioFormat::S16: outputFormat = ma_format_s16; break;
                case AudioFormat::S32: outputFormat = ma_format_s32; break;
                case AudioFormat::F32: outputFormat = ma_format_f32; break;
                default: outputFormat = ma_format_s16; break;
            }
        }

        if (options.forceMono)
        {
            outputChannels = 1;
        }
        else if (options.forceStereo)
        {
            outputChannels = 2;
        }

        if (options.targetSampleRate > 0)
        {
            outputSampleRate = options.targetSampleRate;
        }

        ma_decoder_config config = ma_decoder_config_init(outputFormat, outputChannels, outputSampleRate);
        ma_decoder decoder;

        ma_result result = ma_decoder_init_file(path.c_str(), &config, &decoder);
        if (result != MA_SUCCESS)
        {
            RVX_CORE_WARN("AudioLoader: miniaudio decoder init failed: {}", static_cast<int>(result));
            return false;
        }

        // Get total frames
        ma_uint64 totalFrames;
        result = ma_decoder_get_length_in_pcm_frames(&decoder, &totalFrames);
        if (result != MA_SUCCESS)
        {
            // Some formats don't support getting length, estimate from file size
            totalFrames = 0;
        }

        // Calculate buffer size
        ma_uint32 channels = decoder.outputChannels;
        ma_uint32 sampleRate = decoder.outputSampleRate;
        size_t bytesPerSample = ma_get_bytes_per_sample(decoder.outputFormat);
        size_t bytesPerFrame = bytesPerSample * channels;

        // If we couldn't get total frames, read in chunks
        if (totalFrames == 0)
        {
            const size_t chunkSize = 16384;  // 16K frames per chunk
            std::vector<uint8_t> chunk(chunkSize * bytesPerFrame);
            outData.clear();

            ma_uint64 framesRead;
            while ((result = ma_decoder_read_pcm_frames(&decoder, chunk.data(), chunkSize, &framesRead)) == MA_SUCCESS && framesRead > 0)
            {
                size_t bytesRead = static_cast<size_t>(framesRead) * bytesPerFrame;
                outData.insert(outData.end(), chunk.begin(), chunk.begin() + bytesRead);
                totalFrames += framesRead;
            }
        }
        else
        {
            // Allocate buffer for all data
            outData.resize(static_cast<size_t>(totalFrames) * bytesPerFrame);

            ma_uint64 framesRead;
            result = ma_decoder_read_pcm_frames(&decoder, outData.data(), totalFrames, &framesRead);
            if (result != MA_SUCCESS && result != MA_AT_END)
            {
                ma_decoder_uninit(&decoder);
                return false;
            }

            // Resize to actual frames read
            outData.resize(static_cast<size_t>(framesRead) * bytesPerFrame);
            totalFrames = framesRead;
        }

        // Fill metadata
        outMetadata.sampleRate = sampleRate;
        outMetadata.channels = channels;
        outMetadata.totalFrames = totalFrames;
        outMetadata.duration = static_cast<float>(totalFrames) / static_cast<float>(sampleRate);
        outMetadata.bitsPerSample = static_cast<uint32_t>(bytesPerSample * 8);
        outMetadata.loadMode = AudioLoadMode::FullyLoaded;

        switch (decoder.outputFormat)
        {
            case ma_format_u8:  outMetadata.format = AudioFormat::U8; break;
            case ma_format_s16: outMetadata.format = AudioFormat::S16; break;
            case ma_format_s32: outMetadata.format = AudioFormat::S32; break;
            case ma_format_f32: outMetadata.format = AudioFormat::F32; break;
            default:            outMetadata.format = AudioFormat::S16; break;
        }

        ma_decoder_uninit(&decoder);

        RVX_CORE_INFO("AudioLoader: Decoded {} frames, {}Hz, {}ch", 
            totalFrames, sampleRate, channels);

        return true;
    }

    bool AudioLoader::DecodeMemory(const void* data, size_t size,
                                    std::vector<uint8_t>& outData,
                                    AudioMetadata& outMetadata,
                                    const AudioLoadOptions& options)
    {
        // Configure decoder
        ma_format outputFormat = ma_format_s16;
        ma_uint32 outputChannels = 0;
        ma_uint32 outputSampleRate = 0;

        if (options.targetFormat != AudioFormat::Unknown)
        {
            switch (options.targetFormat)
            {
                case AudioFormat::U8:  outputFormat = ma_format_u8; break;
                case AudioFormat::S16: outputFormat = ma_format_s16; break;
                case AudioFormat::S32: outputFormat = ma_format_s32; break;
                case AudioFormat::F32: outputFormat = ma_format_f32; break;
                default: outputFormat = ma_format_s16; break;
            }
        }

        if (options.forceMono)
        {
            outputChannels = 1;
        }
        else if (options.forceStereo)
        {
            outputChannels = 2;
        }

        if (options.targetSampleRate > 0)
        {
            outputSampleRate = options.targetSampleRate;
        }

        ma_decoder_config config = ma_decoder_config_init(outputFormat, outputChannels, outputSampleRate);
        ma_decoder decoder;

        ma_result result = ma_decoder_init_memory(data, size, &config, &decoder);
        if (result != MA_SUCCESS)
        {
            RVX_CORE_WARN("AudioLoader: miniaudio memory decoder init failed: {}", static_cast<int>(result));
            return false;
        }

        // Get total frames
        ma_uint64 totalFrames = 0;
        ma_decoder_get_length_in_pcm_frames(&decoder, &totalFrames);

        ma_uint32 channels = decoder.outputChannels;
        ma_uint32 sampleRate = decoder.outputSampleRate;
        size_t bytesPerSample = ma_get_bytes_per_sample(decoder.outputFormat);
        size_t bytesPerFrame = bytesPerSample * channels;

        // Read all frames
        if (totalFrames > 0)
        {
            outData.resize(static_cast<size_t>(totalFrames) * bytesPerFrame);
            
            ma_uint64 framesRead;
            ma_decoder_read_pcm_frames(&decoder, outData.data(), totalFrames, &framesRead);
            
            outData.resize(static_cast<size_t>(framesRead) * bytesPerFrame);
            totalFrames = framesRead;
        }
        else
        {
            // Stream read
            const size_t chunkSize = 16384;
            std::vector<uint8_t> chunk(chunkSize * bytesPerFrame);
            outData.clear();

            ma_uint64 framesRead;
            while (ma_decoder_read_pcm_frames(&decoder, chunk.data(), chunkSize, &framesRead) == MA_SUCCESS && framesRead > 0)
            {
                size_t bytesRead = static_cast<size_t>(framesRead) * bytesPerFrame;
                outData.insert(outData.end(), chunk.begin(), chunk.begin() + bytesRead);
                totalFrames += framesRead;
            }
        }

        // Fill metadata
        outMetadata.sampleRate = sampleRate;
        outMetadata.channels = channels;
        outMetadata.totalFrames = totalFrames;
        outMetadata.duration = static_cast<float>(totalFrames) / static_cast<float>(sampleRate);
        outMetadata.bitsPerSample = static_cast<uint32_t>(bytesPerSample * 8);
        outMetadata.loadMode = AudioLoadMode::FullyLoaded;

        switch (decoder.outputFormat)
        {
            case ma_format_u8:  outMetadata.format = AudioFormat::U8; break;
            case ma_format_s16: outMetadata.format = AudioFormat::S16; break;
            case ma_format_s32: outMetadata.format = AudioFormat::S32; break;
            case ma_format_f32: outMetadata.format = AudioFormat::F32; break;
            default:            outMetadata.format = AudioFormat::S16; break;
        }

        ma_decoder_uninit(&decoder);

        return true;
    }

    AudioResource* AudioLoader::CreateAudioResource(std::vector<uint8_t> data,
                                                      const AudioMetadata& metadata,
                                                      const std::string& uniqueKey)
    {
        auto* audio = new AudioResource();

        ResourceId audioId = GenerateAudioId(uniqueKey);
        audio->SetId(audioId);
        audio->SetPath(uniqueKey);

        // Extract name from path
        std::filesystem::path filePath(uniqueKey);
        audio->SetName(filePath.stem().string());

        audio->SetData(std::move(data), metadata);

        // Store in cache
        if (m_manager && m_manager->IsInitialized())
        {
            m_manager->GetCache().Store(audio);
        }

        return audio;
    }

    ResourceId AudioLoader::GenerateAudioId(const std::string& uniqueKey)
    {
        std::hash<std::string> hasher;
        return static_cast<ResourceId>(hasher(uniqueKey));
    }

} // namespace RVX::Resource
