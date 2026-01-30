/**
 * @file AudioStreamer.cpp
 * @brief AudioStreamer implementation
 */

#include "Audio/Streaming/AudioStreamer.h"
#include "Core/Log.h"
#include <miniaudio.h>
#include <cstring>

namespace RVX::Audio
{

AudioStreamer::AudioStreamer()
{
}

AudioStreamer::~AudioStreamer()
{
    Close();
}

void AudioStreamer::SetConfig(const StreamingConfig& config)
{
    if (m_isOpen)
    {
        RVX_CORE_WARN("Cannot change streaming config while open");
        return;
    }
    m_config = config;
}

bool AudioStreamer::Open(const std::string& path)
{
    Close();

    m_path = path;

    // Create decoder
    auto* decoder = new ma_decoder();
    ma_decoder_config decoderConfig = ma_decoder_config_init(ma_format_f32, 0, 0);

    ma_result result = ma_decoder_init_file(path.c_str(), &decoderConfig, decoder);
    if (result != MA_SUCCESS)
    {
        RVX_CORE_ERROR("Failed to open audio file for streaming: {}", path);
        delete decoder;
        return false;
    }

    m_decoder = decoder;

    // Get info
    m_info.sampleRate = decoder->outputSampleRate;
    m_info.channels = decoder->outputChannels;
    m_info.format = AudioFormat::F32;
    m_info.bitsPerSample = 32;

    ma_uint64 totalFrames = 0;
    if (ma_decoder_get_length_in_pcm_frames(decoder, &totalFrames) == MA_SUCCESS)
    {
        m_totalSamples = totalFrames;
        m_info.sampleCount = totalFrames;
        m_info.duration = static_cast<float>(totalFrames) / static_cast<float>(m_info.sampleRate);
    }

    // Allocate buffers
    m_buffers.resize(m_config.bufferCount);
    for (auto& buffer : m_buffers)
    {
        buffer.data.resize(m_config.bufferSize);
        buffer.filled = 0;
        buffer.read = 0;
        buffer.ready = false;
    }
    m_writeBuffer = 0;
    m_readBuffer = 0;
    m_currentSample = 0;

    m_isOpen = true;
    SetState(StreamingState::Loading);

    // Fill initial buffers
    for (size_t i = 0; i < static_cast<size_t>(m_config.bufferCount); ++i)
    {
        if (!FillBuffer(i))
        {
            break;
        }
    }

    // Start prefetch thread
    if (m_config.enablePrefetch)
    {
        m_stopPrefetch = false;
        m_prefetchThread = std::thread(&AudioStreamer::PrefetchLoop, this);
    }

    SetState(StreamingState::Streaming);
    RVX_CORE_INFO("Opened audio stream: {} ({:.2f}s)", path, m_info.duration);

    return true;
}

bool AudioStreamer::Open(AudioClip::Ptr clip)
{
    if (!clip || !clip->IsStreaming())
    {
        RVX_CORE_WARN("Clip must be streaming-enabled for AudioStreamer");
        return false;
    }

    return Open(clip->GetPath());
}

void AudioStreamer::Close()
{
    if (!m_isOpen)
    {
        return;
    }

    // Stop prefetch thread
    m_stopPrefetch = true;
    if (m_prefetchThread.joinable())
    {
        m_prefetchThread.join();
    }

    // Clean up decoder
    if (m_decoder)
    {
        ma_decoder_uninit(static_cast<ma_decoder*>(m_decoder));
        delete static_cast<ma_decoder*>(m_decoder);
        m_decoder = nullptr;
    }

    // Clear buffers
    m_buffers.clear();

    m_isOpen = false;
    SetState(StreamingState::Idle);

    RVX_CORE_INFO("Closed audio stream: {}", m_path);
}

size_t AudioStreamer::Read(void* buffer, size_t maxBytes)
{
    if (!m_isOpen || !buffer)
    {
        return 0;
    }

    std::lock_guard<std::mutex> lock(m_bufferMutex);

    size_t totalRead = 0;
    uint8* outPtr = static_cast<uint8*>(buffer);

    while (totalRead < maxBytes && m_buffers[m_readBuffer].ready)
    {
        auto& buf = m_buffers[m_readBuffer];
        size_t available = buf.filled - buf.read;
        size_t toRead = std::min(available, maxBytes - totalRead);

        if (toRead > 0)
        {
            std::memcpy(outPtr + totalRead, buf.data.data() + buf.read, toRead);
            buf.read += toRead;
            totalRead += toRead;

            // Update sample position
            size_t bytesPerSample = m_info.channels * sizeof(float);
            m_currentSample += toRead / bytesPerSample;
        }

        // Move to next buffer if this one is exhausted
        if (buf.read >= buf.filled)
        {
            buf.ready = false;
            buf.filled = 0;
            buf.read = 0;
            m_readBuffer = (m_readBuffer + 1) % m_buffers.size();

            // Check if we've reached the end
            if (!m_buffers[m_readBuffer].ready && m_currentSample >= m_totalSamples)
            {
                SetState(StreamingState::Finished);
                break;
            }
        }
    }

    return totalRead;
}

size_t AudioStreamer::GetAvailableBytes() const
{
    if (!m_isOpen)
    {
        return 0;
    }

    std::lock_guard<std::mutex> lock(m_bufferMutex);

    size_t available = 0;
    size_t idx = m_readBuffer;

    for (size_t i = 0; i < m_buffers.size(); ++i)
    {
        const auto& buf = m_buffers[idx];
        if (buf.ready)
        {
            available += buf.filled - buf.read;
        }
        idx = (idx + 1) % m_buffers.size();
    }

    return available;
}

bool AudioStreamer::Seek(float timeSeconds)
{
    if (!m_isOpen || !m_decoder)
    {
        return false;
    }

    uint64 targetSample = static_cast<uint64>(timeSeconds * m_info.sampleRate);
    return SeekToSample(targetSample);
}

bool AudioStreamer::SeekToSample(uint64 sample)
{
    if (!m_isOpen || !m_decoder)
    {
        return false;
    }

    sample = std::min(sample, static_cast<uint64>(m_totalSamples.load()));

    m_seekTarget = sample;
    m_seekRequested = true;

    // Wait for seek to complete (or do it synchronously if no prefetch)
    if (!m_config.enablePrefetch)
    {
        auto* decoder = static_cast<ma_decoder*>(m_decoder);
        ma_decoder_seek_to_pcm_frame(decoder, sample);
        m_currentSample = sample;

        ClearBuffers();

        // Refill buffers
        for (size_t i = 0; i < static_cast<size_t>(m_config.bufferCount); ++i)
        {
            if (!FillBuffer(i))
            {
                break;
            }
        }
    }

    return true;
}

float AudioStreamer::GetPosition() const
{
    return static_cast<float>(m_currentSample.load()) / static_cast<float>(m_info.sampleRate);
}

void AudioStreamer::SetStateCallback(StreamingCallback callback)
{
    m_stateCallback = std::move(callback);
}

void AudioStreamer::Update()
{
    // For non-threaded operation, fill buffers here
    if (!m_config.enablePrefetch && m_isOpen)
    {
        std::lock_guard<std::mutex> lock(m_bufferMutex);

        // Find empty buffers and fill them
        for (size_t i = 0; i < m_buffers.size(); ++i)
        {
            if (!m_buffers[i].ready)
            {
                FillBuffer(i);
            }
        }
    }
}

void AudioStreamer::SetState(StreamingState state)
{
    StreamingState oldState = m_state.exchange(state);
    if (oldState != state && m_stateCallback)
    {
        m_stateCallback(state);
    }
}

void AudioStreamer::PrefetchLoop()
{
    while (!m_stopPrefetch)
    {
        // Handle seek request
        if (m_seekRequested)
        {
            m_seekRequested = false;
            uint64 target = m_seekTarget;

            auto* decoder = static_cast<ma_decoder*>(m_decoder);
            ma_decoder_seek_to_pcm_frame(decoder, target);
            m_currentSample = target;

            std::lock_guard<std::mutex> lock(m_bufferMutex);
            ClearBuffers();

            // Refill buffers
            for (size_t i = 0; i < static_cast<size_t>(m_config.bufferCount); ++i)
            {
                if (!FillBuffer(i))
                {
                    break;
                }
            }

            if (m_state == StreamingState::Finished)
            {
                SetState(StreamingState::Streaming);
            }
        }

        // Count ready buffers
        size_t readyCount = 0;
        {
            std::lock_guard<std::mutex> lock(m_bufferMutex);
            for (const auto& buf : m_buffers)
            {
                if (buf.ready)
                {
                    readyCount++;
                }
            }
        }

        // Fill buffers if below threshold
        if (readyCount < m_config.prefetchThreshold)
        {
            std::lock_guard<std::mutex> lock(m_bufferMutex);

            size_t idx = m_writeBuffer;
            for (size_t i = 0; i < m_buffers.size() && readyCount < m_buffers.size(); ++i)
            {
                if (!m_buffers[idx].ready)
                {
                    if (FillBuffer(idx))
                    {
                        readyCount++;
                        m_writeBuffer = (idx + 1) % m_buffers.size();
                    }
                    else
                    {
                        break;  // End of stream or error
                    }
                }
                idx = (idx + 1) % m_buffers.size();
            }
        }

        // Sleep briefly
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

bool AudioStreamer::FillBuffer(size_t bufferIndex)
{
    if (!m_decoder || bufferIndex >= m_buffers.size())
    {
        return false;
    }

    auto* decoder = static_cast<ma_decoder*>(m_decoder);
    auto& buf = m_buffers[bufferIndex];

    size_t bytesPerFrame = m_info.channels * sizeof(float);
    size_t framesToRead = m_config.bufferSize / bytesPerFrame;

    ma_uint64 framesRead = 0;
    ma_result result = ma_decoder_read_pcm_frames(
        decoder,
        buf.data.data(),
        framesToRead,
        &framesRead
    );

    if (result != MA_SUCCESS && result != MA_AT_END)
    {
        SetState(StreamingState::Error);
        return false;
    }

    buf.filled = static_cast<size_t>(framesRead * bytesPerFrame);
    buf.read = 0;
    buf.ready = (buf.filled > 0);

    if (framesRead < framesToRead)
    {
        // Reached end of stream
        return false;
    }

    return true;
}

void AudioStreamer::ClearBuffers()
{
    for (auto& buf : m_buffers)
    {
        buf.filled = 0;
        buf.read = 0;
        buf.ready = false;
    }
    m_writeBuffer = 0;
    m_readBuffer = 0;
}

} // namespace RVX::Audio
