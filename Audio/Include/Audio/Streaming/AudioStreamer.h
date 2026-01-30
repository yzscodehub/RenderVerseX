/**
 * @file AudioStreamer.h
 * @brief Audio streaming for large files
 */

#pragma once

#include "Audio/AudioTypes.h"
#include "Audio/AudioClip.h"
#include <memory>
#include <vector>
#include <mutex>
#include <atomic>
#include <thread>
#include <functional>

namespace RVX::Audio
{

/**
 * @brief Streaming state
 */
enum class StreamingState : uint8
{
    Idle,           ///< Not streaming
    Loading,        ///< Initial buffer loading
    Streaming,      ///< Actively streaming
    Buffering,      ///< Rebuffering (buffer underrun)
    Finished,       ///< Reached end of file
    Error           ///< Streaming error
};

/**
 * @brief Streaming buffer configuration
 */
struct StreamingConfig
{
    size_t bufferSize = 65536;       ///< Size of each buffer in bytes
    int bufferCount = 4;             ///< Number of buffers (ring buffer)
    size_t prefetchThreshold = 2;    ///< Prefetch when this many buffers available
    bool enablePrefetch = true;      ///< Enable background prefetching
};

/**
 * @brief Callback for streaming events
 */
using StreamingCallback = std::function<void(StreamingState state)>;

/**
 * @brief Audio streamer for large files
 * 
 * Handles background loading and buffering of large audio files
 * that shouldn't be fully loaded into memory.
 * 
 * Features:
 * - Ring buffer for smooth playback
 * - Background prefetching
 * - Seek support
 * - Buffer underrun detection
 * 
 * Usage:
 * @code
 * AudioStreamer streamer;
 * streamer.Open("large_audio.ogg");
 * 
 * while (playing)
 * {
 *     size_t available = streamer.GetAvailableBytes();
 *     if (available > 0)
 *     {
 *         size_t read = streamer.Read(buffer, bufferSize);
 *         // Send to audio device
 *     }
 * }
 * @endcode
 */
class AudioStreamer
{
public:
    AudioStreamer();
    ~AudioStreamer();

    // Non-copyable
    AudioStreamer(const AudioStreamer&) = delete;
    AudioStreamer& operator=(const AudioStreamer&) = delete;

    // =========================================================================
    // Initialization
    // =========================================================================

    /**
     * @brief Set streaming configuration (before Open)
     */
    void SetConfig(const StreamingConfig& config);
    const StreamingConfig& GetConfig() const { return m_config; }

    /**
     * @brief Open a file for streaming
     */
    bool Open(const std::string& path);

    /**
     * @brief Open from an AudioClip (must be streaming-enabled)
     */
    bool Open(AudioClip::Ptr clip);

    /**
     * @brief Close the streamer
     */
    void Close();

    /**
     * @brief Check if open
     */
    bool IsOpen() const { return m_isOpen; }

    // =========================================================================
    // Reading
    // =========================================================================

    /**
     * @brief Read decoded audio data
     * @param buffer Output buffer for PCM data
     * @param maxBytes Maximum bytes to read
     * @return Number of bytes actually read
     */
    size_t Read(void* buffer, size_t maxBytes);

    /**
     * @brief Get number of bytes available for reading
     */
    size_t GetAvailableBytes() const;

    /**
     * @brief Check if more data is available
     */
    bool HasData() const { return GetAvailableBytes() > 0; }

    // =========================================================================
    // Seeking
    // =========================================================================

    /**
     * @brief Seek to a position in seconds
     */
    bool Seek(float timeSeconds);

    /**
     * @brief Seek to a sample position
     */
    bool SeekToSample(uint64 sample);

    /**
     * @brief Get current position in seconds
     */
    float GetPosition() const;

    /**
     * @brief Get current sample position
     */
    uint64 GetSamplePosition() const { return m_currentSample; }

    // =========================================================================
    // State
    // =========================================================================

    StreamingState GetState() const { return m_state; }
    bool IsFinished() const { return m_state == StreamingState::Finished; }
    bool HasError() const { return m_state == StreamingState::Error; }

    /**
     * @brief Set callback for state changes
     */
    void SetStateCallback(StreamingCallback callback);

    // =========================================================================
    // Info
    // =========================================================================

    const AudioClipInfo& GetInfo() const { return m_info; }
    float GetDuration() const { return m_info.duration; }
    uint32 GetSampleRate() const { return m_info.sampleRate; }
    uint32 GetChannels() const { return m_info.channels; }

    // =========================================================================
    // Update
    // =========================================================================

    /**
     * @brief Update streamer (call periodically to trigger prefetch)
     */
    void Update();

private:
    StreamingConfig m_config;
    bool m_isOpen = false;
    std::atomic<StreamingState> m_state{StreamingState::Idle};
    
    // File info
    std::string m_path;
    AudioClipInfo m_info;
    
    // Decoder (miniaudio)
    void* m_decoder = nullptr;
    
    // Ring buffer
    struct Buffer
    {
        std::vector<uint8> data;
        size_t filled = 0;
        size_t read = 0;
        bool ready = false;
    };
    std::vector<Buffer> m_buffers;
    size_t m_writeBuffer = 0;
    size_t m_readBuffer = 0;
    mutable std::mutex m_bufferMutex;
    
    // Position tracking
    std::atomic<uint64> m_currentSample{0};
    std::atomic<uint64> m_totalSamples{0};
    
    // Background thread
    std::thread m_prefetchThread;
    std::atomic<bool> m_stopPrefetch{false};
    std::atomic<bool> m_seekRequested{false};
    std::atomic<uint64> m_seekTarget{0};
    
    // Callback
    StreamingCallback m_stateCallback;

    void SetState(StreamingState state);
    void PrefetchLoop();
    bool FillBuffer(size_t bufferIndex);
    void ClearBuffers();
};

} // namespace RVX::Audio
