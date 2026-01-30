/**
 * @file AudioClip.cpp
 * @brief AudioClip implementation with miniaudio decoder
 */

#include "Audio/AudioClip.h"
#include "Core/Log.h"
#include <miniaudio.h>
#include <fstream>
#include <cstring>

namespace RVX::Audio
{

/**
 * @brief Internal decoder data for the clip
 */
struct AudioClipBackend
{
    ma_decoder decoder;
    bool decoderInitialized = false;
    std::vector<uint8> encodedData;  // For memory-loaded clips
};

AudioClip::~AudioClip()
{
    Unload();
}

bool AudioClip::LoadFromFile(const std::string& path)
{
    if (m_loaded)
    {
        Unload();
    }

    m_path = path;

    // Extract name from path
    size_t lastSlash = path.find_last_of("/\\");
    m_name = (lastSlash != std::string::npos) ? path.substr(lastSlash + 1) : path;

    // Create backend data
    auto backend = std::make_unique<AudioClipBackend>();

    // Configure decoder
    ma_decoder_config decoderConfig = ma_decoder_config_init(ma_format_f32, 0, 0);

    // Initialize decoder from file
    ma_result result = ma_decoder_init_file(path.c_str(), &decoderConfig, &backend->decoder);
    if (result != MA_SUCCESS)
    {
        RVX_CORE_ERROR("Failed to load audio file '{}': error {}", path, static_cast<int>(result));
        return false;
    }
    backend->decoderInitialized = true;

    // Get audio info
    m_info.sampleRate = backend->decoder.outputSampleRate;
    m_info.channels = backend->decoder.outputChannels;
    m_info.format = AudioFormat::F32;
    m_info.bitsPerSample = 32;

    // Get total frame count
    ma_uint64 totalFrames = 0;
    result = ma_decoder_get_length_in_pcm_frames(&backend->decoder, &totalFrames);
    if (result == MA_SUCCESS)
    {
        m_info.sampleCount = totalFrames;
        m_info.duration = static_cast<float>(totalFrames) / static_cast<float>(m_info.sampleRate);
    }
    else
    {
        // Some formats (like streaming) might not report length
        m_info.sampleCount = 0;
        m_info.duration = 0.0f;
    }

    // For non-streaming clips, decode the entire audio into memory
    if (!m_streaming && m_info.sampleCount > 0)
    {
        size_t dataSize = static_cast<size_t>(m_info.sampleCount * m_info.channels * sizeof(float));
        m_data.resize(dataSize);

        ma_uint64 framesRead = 0;
        result = ma_decoder_read_pcm_frames(&backend->decoder, m_data.data(), m_info.sampleCount, &framesRead);
        if (result != MA_SUCCESS && result != MA_AT_END)
        {
            RVX_CORE_WARN("Partial read for audio file '{}': read {} frames", path, framesRead);
        }

        // Reset decoder position for potential re-read
        ma_decoder_seek_to_pcm_frame(&backend->decoder, 0);
    }

    m_backendData = backend.release();
    m_loaded = true;

    RVX_CORE_INFO("Loaded audio clip '{}' ({:.2f}s, {}Hz, {} channels)",
        m_name, m_info.duration, m_info.sampleRate, m_info.channels);

    return true;
}

bool AudioClip::LoadFromMemory(const void* data, size_t size)
{
    if (!data || size == 0)
    {
        RVX_CORE_ERROR("Invalid audio data (null or zero size)");
        return false;
    }

    if (m_loaded)
    {
        Unload();
    }

    // Create backend data
    auto backend = std::make_unique<AudioClipBackend>();

    // Store the encoded data for the decoder to use
    backend->encodedData.resize(size);
    std::memcpy(backend->encodedData.data(), data, size);

    // Configure decoder
    ma_decoder_config decoderConfig = ma_decoder_config_init(ma_format_f32, 0, 0);

    // Initialize decoder from memory
    ma_result result = ma_decoder_init_memory(
        backend->encodedData.data(),
        backend->encodedData.size(),
        &decoderConfig,
        &backend->decoder);

    if (result != MA_SUCCESS)
    {
        RVX_CORE_ERROR("Failed to load audio from memory: error {}", static_cast<int>(result));
        return false;
    }
    backend->decoderInitialized = true;

    // Get audio info
    m_info.sampleRate = backend->decoder.outputSampleRate;
    m_info.channels = backend->decoder.outputChannels;
    m_info.format = AudioFormat::F32;
    m_info.bitsPerSample = 32;

    // Get total frame count
    ma_uint64 totalFrames = 0;
    result = ma_decoder_get_length_in_pcm_frames(&backend->decoder, &totalFrames);
    if (result == MA_SUCCESS)
    {
        m_info.sampleCount = totalFrames;
        m_info.duration = static_cast<float>(totalFrames) / static_cast<float>(m_info.sampleRate);
    }

    // For non-streaming clips, decode into PCM
    if (!m_streaming && m_info.sampleCount > 0)
    {
        size_t dataSize = static_cast<size_t>(m_info.sampleCount * m_info.channels * sizeof(float));
        m_data.resize(dataSize);

        ma_uint64 framesRead = 0;
        ma_decoder_read_pcm_frames(&backend->decoder, m_data.data(), m_info.sampleCount, &framesRead);
        ma_decoder_seek_to_pcm_frame(&backend->decoder, 0);
    }

    m_backendData = backend.release();
    m_loaded = true;
    m_name = "memory_audio";

    RVX_CORE_INFO("Loaded audio clip from memory ({:.2f}s, {}Hz, {} channels)",
        m_info.duration, m_info.sampleRate, m_info.channels);

    return true;
}

void AudioClip::Unload()
{
    if (m_backendData)
    {
        auto* backend = static_cast<AudioClipBackend*>(m_backendData);
        if (backend)
        {
            if (backend->decoderInitialized)
            {
                ma_decoder_uninit(&backend->decoder);
            }
            delete backend;
        }
        m_backendData = nullptr;
    }

    m_data.clear();
    m_data.shrink_to_fit();
    m_loaded = false;
    m_info = AudioClipInfo{};

    RVX_CORE_DEBUG("Unloaded audio clip '{}'", m_name);
}

AudioClip::Ptr AudioClip::Create(const std::string& path)
{
    auto clip = std::make_shared<AudioClip>();
    if (clip->LoadFromFile(path))
    {
        return clip;
    }
    return nullptr;
}

} // namespace RVX::Audio
