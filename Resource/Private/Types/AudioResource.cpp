#include "Resource/Types/AudioResource.h"
#include "Core/Log.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace RVX::Resource
{
    // =========================================================================
    // Construction
    // =========================================================================

    AudioResource::AudioResource()
    {
    }

    AudioResource::~AudioResource()
    {
    }

    // =========================================================================
    // Resource Interface
    // =========================================================================

    size_t AudioResource::GetMemoryUsage() const
    {
        return sizeof(*this) + m_data.size() + m_streamingPath.size();
    }

    // =========================================================================
    // Data Management
    // =========================================================================

    void AudioResource::SetData(std::vector<uint8_t> data, const AudioMetadata& metadata)
    {
        m_data = std::move(data);
        m_metadata = metadata;
        
        // Calculate duration if not set
        if (m_metadata.duration <= 0.0f && m_metadata.totalFrames > 0)
        {
            m_metadata.duration = FramesToDuration(m_metadata.totalFrames, m_metadata.sampleRate);
        }

        // Set loop end to total frames if not set
        if (m_metadata.loopEndFrame == 0)
        {
            m_metadata.loopEndFrame = m_metadata.totalFrames;
        }

        SetState(ResourceState::Loaded);
        NotifyLoaded();
    }

    void AudioResource::SetLoopPoints(uint64_t startFrame, uint64_t endFrame)
    {
        m_metadata.loopStartFrame = std::min(startFrame, m_metadata.totalFrames);
        m_metadata.loopEndFrame = std::min(endFrame, m_metadata.totalFrames);
        
        if (m_metadata.loopEndFrame <= m_metadata.loopStartFrame)
        {
            m_metadata.loopEndFrame = m_metadata.totalFrames;
        }
    }

    void AudioResource::SetStreamingSourcePath(const std::string& path)
    {
        m_streamingPath = path;
        m_metadata.loadMode = AudioLoadMode::Streaming;
    }

    // =========================================================================
    // Format Helpers
    // =========================================================================

    size_t AudioResource::GetBytesPerFrame() const
    {
        return GetBytesPerSample() * m_metadata.channels;
    }

    size_t AudioResource::GetBytesPerSample() const
    {
        return GetAudioFormatBytes(m_metadata.format);
    }

    // =========================================================================
    // Format Conversion
    // =========================================================================

    bool AudioResource::ConvertToFormat(AudioFormat targetFormat)
    {
        if (m_metadata.format == targetFormat)
        {
            return true;
        }

        if (m_data.empty())
        {
            return false;
        }

        size_t srcBytesPerSample = GetAudioFormatBytes(m_metadata.format);
        size_t dstBytesPerSample = GetAudioFormatBytes(targetFormat);
        
        size_t numSamples = m_data.size() / srcBytesPerSample;
        std::vector<uint8_t> newData(numSamples * dstBytesPerSample);

        // Convert via float intermediary
        for (size_t i = 0; i < numSamples; ++i)
        {
            float sample = 0.0f;

            // Read source sample as float
            switch (m_metadata.format)
            {
                case AudioFormat::U8:
                {
                    uint8_t u8 = m_data[i];
                    sample = (static_cast<float>(u8) - 128.0f) / 128.0f;
                    break;
                }
                case AudioFormat::S16:
                {
                    int16_t s16;
                    std::memcpy(&s16, &m_data[i * 2], 2);
                    sample = static_cast<float>(s16) / 32768.0f;
                    break;
                }
                case AudioFormat::S32:
                {
                    int32_t s32;
                    std::memcpy(&s32, &m_data[i * 4], 4);
                    sample = static_cast<float>(s32) / 2147483648.0f;
                    break;
                }
                case AudioFormat::F32:
                {
                    std::memcpy(&sample, &m_data[i * 4], 4);
                    break;
                }
                default:
                    break;
            }

            // Clamp
            sample = std::clamp(sample, -1.0f, 1.0f);

            // Write to target format
            switch (targetFormat)
            {
                case AudioFormat::U8:
                {
                    uint8_t u8 = static_cast<uint8_t>((sample + 1.0f) * 127.5f);
                    newData[i] = u8;
                    break;
                }
                case AudioFormat::S16:
                {
                    int16_t s16 = static_cast<int16_t>(sample * 32767.0f);
                    std::memcpy(&newData[i * 2], &s16, 2);
                    break;
                }
                case AudioFormat::S32:
                {
                    int32_t s32 = static_cast<int32_t>(sample * 2147483647.0f);
                    std::memcpy(&newData[i * 4], &s32, 4);
                    break;
                }
                case AudioFormat::F32:
                {
                    std::memcpy(&newData[i * 4], &sample, 4);
                    break;
                }
                default:
                    break;
            }
        }

        m_data = std::move(newData);
        m_metadata.format = targetFormat;
        m_metadata.bitsPerSample = static_cast<uint32_t>(dstBytesPerSample * 8);

        return true;
    }

    bool AudioResource::ConvertToMono()
    {
        if (m_metadata.channels == 1)
        {
            return true;
        }

        if (m_data.empty())
        {
            return false;
        }

        size_t bytesPerSample = GetBytesPerSample();
        size_t bytesPerFrame = bytesPerSample * m_metadata.channels;
        size_t numFrames = m_data.size() / bytesPerFrame;

        std::vector<uint8_t> newData(numFrames * bytesPerSample);

        for (size_t frame = 0; frame < numFrames; ++frame)
        {
            float sum = 0.0f;

            for (uint32_t ch = 0; ch < m_metadata.channels; ++ch)
            {
                size_t offset = frame * bytesPerFrame + ch * bytesPerSample;
                float sample = 0.0f;

                switch (m_metadata.format)
                {
                    case AudioFormat::S16:
                    {
                        int16_t s16;
                        std::memcpy(&s16, &m_data[offset], 2);
                        sample = static_cast<float>(s16) / 32768.0f;
                        break;
                    }
                    case AudioFormat::F32:
                    {
                        std::memcpy(&sample, &m_data[offset], 4);
                        break;
                    }
                    default:
                        break;
                }

                sum += sample;
            }

            float avg = sum / static_cast<float>(m_metadata.channels);
            size_t dstOffset = frame * bytesPerSample;

            switch (m_metadata.format)
            {
                case AudioFormat::S16:
                {
                    int16_t s16 = static_cast<int16_t>(std::clamp(avg, -1.0f, 1.0f) * 32767.0f);
                    std::memcpy(&newData[dstOffset], &s16, 2);
                    break;
                }
                case AudioFormat::F32:
                {
                    std::memcpy(&newData[dstOffset], &avg, 4);
                    break;
                }
                default:
                    break;
            }
        }

        m_data = std::move(newData);
        m_metadata.channels = 1;

        return true;
    }

    bool AudioResource::Resample(uint32_t targetSampleRate)
    {
        if (m_metadata.sampleRate == targetSampleRate)
        {
            return true;
        }

        if (m_data.empty())
        {
            return false;
        }

        // Simple linear interpolation resampling
        double ratio = static_cast<double>(targetSampleRate) / static_cast<double>(m_metadata.sampleRate);
        size_t bytesPerFrame = GetBytesPerFrame();
        size_t sourceFrames = m_data.size() / bytesPerFrame;
        size_t targetFrames = static_cast<size_t>(sourceFrames * ratio);

        std::vector<uint8_t> newData(targetFrames * bytesPerFrame);

        // For simplicity, convert to float, resample, convert back
        // This implementation handles S16 stereo which is most common

        if (m_metadata.format == AudioFormat::S16)
        {
            for (size_t i = 0; i < targetFrames; ++i)
            {
                double sourcePos = i / ratio;
                size_t sourceIdx = static_cast<size_t>(sourcePos);
                double frac = sourcePos - sourceIdx;

                if (sourceIdx + 1 >= sourceFrames)
                {
                    sourceIdx = sourceFrames - 1;
                    frac = 0.0;
                }

                for (uint32_t ch = 0; ch < m_metadata.channels; ++ch)
                {
                    size_t offset0 = (sourceIdx * m_metadata.channels + ch) * 2;
                    size_t offset1 = ((sourceIdx + 1) * m_metadata.channels + ch) * 2;

                    int16_t s0, s1;
                    std::memcpy(&s0, &m_data[offset0], 2);
                    std::memcpy(&s1, &m_data[std::min(offset1, m_data.size() - 2)], 2);

                    int16_t result = static_cast<int16_t>(s0 * (1.0 - frac) + s1 * frac);

                    size_t dstOffset = (i * m_metadata.channels + ch) * 2;
                    std::memcpy(&newData[dstOffset], &result, 2);
                }
            }
        }

        m_data = std::move(newData);
        m_metadata.sampleRate = targetSampleRate;
        m_metadata.totalFrames = targetFrames;
        m_metadata.duration = FramesToDuration(targetFrames, targetSampleRate);

        return true;
    }

    // =========================================================================
    // Streaming
    // =========================================================================

    std::unique_ptr<AudioStreamBuffer> AudioResource::CreateStreamBuffer() const
    {
        // For streaming, we need miniaudio decoder
        // This is a placeholder - actual implementation would use miniaudio
        if (!IsStreaming() || m_streamingPath.empty())
        {
            return nullptr;
        }

        // Return null for now - will be implemented with miniaudio integration
        RVX_CORE_WARN("AudioResource: Streaming not yet implemented");
        return nullptr;
    }

    // =========================================================================
    // Utility Functions
    // =========================================================================

    const char* GetAudioFormatName(AudioFormat format)
    {
        switch (format)
        {
            case AudioFormat::U8:  return "U8";
            case AudioFormat::S16: return "S16";
            case AudioFormat::S24: return "S24";
            case AudioFormat::S32: return "S32";
            case AudioFormat::F32: return "F32";
            default:               return "Unknown";
        }
    }

    size_t GetAudioFormatBytes(AudioFormat format)
    {
        switch (format)
        {
            case AudioFormat::U8:  return 1;
            case AudioFormat::S16: return 2;
            case AudioFormat::S24: return 3;
            case AudioFormat::S32: return 4;
            case AudioFormat::F32: return 4;
            default:               return 0;
        }
    }

    uint64_t DurationToFrames(float duration, uint32_t sampleRate)
    {
        return static_cast<uint64_t>(duration * static_cast<float>(sampleRate));
    }

    float FramesToDuration(uint64_t frames, uint32_t sampleRate)
    {
        return static_cast<float>(frames) / static_cast<float>(sampleRate);
    }

} // namespace RVX::Resource
