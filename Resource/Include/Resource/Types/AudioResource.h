#pragma once

/**
 * @file AudioResource.h
 * @brief Audio resource type
 * 
 * Represents audio data loaded from WAV, OGG, MP3, FLAC files.
 * Supports both fully loaded and streaming modes.
 */

#include "Resource/IResource.h"
#include <cstdint>
#include <vector>
#include <memory>

namespace RVX::Resource
{
    /**
     * @brief Audio sample format
     */
    enum class AudioFormat : uint8_t
    {
        Unknown,
        U8,         // Unsigned 8-bit
        S16,        // Signed 16-bit (most common)
        S24,        // Signed 24-bit
        S32,        // Signed 32-bit
        F32         // 32-bit float
    };

    /**
     * @brief Audio loading mode
     */
    enum class AudioLoadMode : uint8_t
    {
        FullyLoaded,    // Load entire audio into memory (for short sounds)
        Streaming       // Stream from disk (for music/long audio)
    };

    /**
     * @brief Audio metadata
     */
    struct AudioMetadata
    {
        uint32_t sampleRate = 44100;        // Sample rate in Hz
        uint32_t channels = 2;              // Number of channels (1=mono, 2=stereo)
        uint32_t bitsPerSample = 16;        // Bits per sample
        uint64_t totalFrames = 0;           // Total number of frames
        float duration = 0.0f;              // Duration in seconds
        AudioFormat format = AudioFormat::S16;
        AudioLoadMode loadMode = AudioLoadMode::FullyLoaded;

        /// Source file format
        std::string sourceFormat;  // "wav", "ogg", "mp3", "flac"

        /// Whether audio should loop
        bool isLooping = false;

        /// Loop points (in frames)
        uint64_t loopStartFrame = 0;
        uint64_t loopEndFrame = 0;
    };

    /**
     * @brief Streaming buffer for audio playback
     */
    class AudioStreamBuffer
    {
    public:
        virtual ~AudioStreamBuffer() = default;

        /// Read frames from the stream
        /// @return Number of frames actually read
        virtual uint64_t Read(void* outBuffer, uint64_t frameCount) = 0;

        /// Seek to a specific frame
        virtual bool Seek(uint64_t frameIndex) = 0;

        /// Get current position in frames
        virtual uint64_t GetPosition() const = 0;

        /// Check if end of stream reached
        virtual bool IsEndOfStream() const = 0;

        /// Reset stream to beginning
        virtual void Reset() = 0;
    };

    /**
     * @brief Audio resource - encapsulates audio data
     * 
     * For short sounds (SFX), audio is fully loaded into memory.
     * For long audio (music), streaming mode can be used.
     */
    class AudioResource : public IResource
    {
    public:
        AudioResource();
        ~AudioResource() override;

        // =====================================================================
        // Resource Interface
        // =====================================================================

        ResourceType GetType() const override { return ResourceType::Audio; }
        const char* GetTypeName() const override { return "Audio"; }
        size_t GetMemoryUsage() const override;

        // =====================================================================
        // Metadata
        // =====================================================================

        const AudioMetadata& GetMetadata() const { return m_metadata; }
        uint32_t GetSampleRate() const { return m_metadata.sampleRate; }
        uint32_t GetChannels() const { return m_metadata.channels; }
        uint64_t GetTotalFrames() const { return m_metadata.totalFrames; }
        float GetDuration() const { return m_metadata.duration; }
        AudioFormat GetFormat() const { return m_metadata.format; }
        AudioLoadMode GetLoadMode() const { return m_metadata.loadMode; }

        bool IsStreaming() const { return m_metadata.loadMode == AudioLoadMode::Streaming; }
        bool IsLooping() const { return m_metadata.isLooping; }

        void SetLooping(bool looping) { m_metadata.isLooping = looping; }
        void SetLoopPoints(uint64_t startFrame, uint64_t endFrame);

        // =====================================================================
        // Data Access (for fully loaded audio)
        // =====================================================================

        /// Get raw audio data (only valid for FullyLoaded mode)
        const std::vector<uint8_t>& GetData() const { return m_data; }

        /// Get data pointer (only valid for FullyLoaded mode)
        const void* GetDataPtr() const { return m_data.data(); }

        /// Get data size in bytes
        size_t GetDataSize() const { return m_data.size(); }

        /// Set audio data
        void SetData(std::vector<uint8_t> data, const AudioMetadata& metadata);

        // =====================================================================
        // Streaming Interface
        // =====================================================================

        /// Create a stream buffer for this audio (for streaming mode)
        /// Multiple stream buffers can be created for the same resource
        std::unique_ptr<AudioStreamBuffer> CreateStreamBuffer() const;

        /// Set the streaming source path (for lazy loading)
        void SetStreamingSourcePath(const std::string& path);
        const std::string& GetStreamingSourcePath() const { return m_streamingPath; }

        // =====================================================================
        // Format Conversion
        // =====================================================================

        /// Convert to different sample format
        bool ConvertToFormat(AudioFormat targetFormat);

        /// Resample to different sample rate
        bool Resample(uint32_t targetSampleRate);

        /// Convert to mono
        bool ConvertToMono();

        /// Get bytes per frame
        size_t GetBytesPerFrame() const;

        /// Get bytes per sample
        size_t GetBytesPerSample() const;

    private:
        AudioMetadata m_metadata;
        std::vector<uint8_t> m_data;        // Raw audio data (for fully loaded)
        std::string m_streamingPath;         // Path for streaming source
    };

    // =========================================================================
    // Utility Functions
    // =========================================================================

    /// Get format name
    const char* GetAudioFormatName(AudioFormat format);

    /// Get bytes per sample for format
    size_t GetAudioFormatBytes(AudioFormat format);

    /// Convert duration to frame count
    uint64_t DurationToFrames(float duration, uint32_t sampleRate);

    /// Convert frame count to duration
    float FramesToDuration(uint64_t frames, uint32_t sampleRate);

} // namespace RVX::Resource
