/**
 * @file AudioClip.h
 * @brief Audio clip resource
 */

#pragma once

#include "Audio/AudioTypes.h"
#include <memory>
#include <string>
#include <vector>

namespace RVX::Audio
{

/**
 * @brief Audio clip resource
 * 
 * Represents loaded audio data that can be played through the AudioEngine.
 */
class AudioClip
{
public:
    using Ptr = std::shared_ptr<AudioClip>;
    using ConstPtr = std::shared_ptr<const AudioClip>;

    AudioClip() = default;
    ~AudioClip();

    // Non-copyable
    AudioClip(const AudioClip&) = delete;
    AudioClip& operator=(const AudioClip&) = delete;

    // =========================================================================
    // Loading
    // =========================================================================

    /**
     * @brief Load from file
     */
    bool LoadFromFile(const std::string& path);

    /**
     * @brief Load from memory
     */
    bool LoadFromMemory(const void* data, size_t size);

    /**
     * @brief Unload audio data
     */
    void Unload();

    /**
     * @brief Check if loaded
     */
    bool IsLoaded() const { return m_loaded; }

    // =========================================================================
    // Properties
    // =========================================================================

    const std::string& GetPath() const { return m_path; }
    const std::string& GetName() const { return m_name; }
    void SetName(const std::string& name) { m_name = name; }

    const AudioClipInfo& GetInfo() const { return m_info; }

    float GetDuration() const { return m_info.duration; }
    uint32 GetSampleRate() const { return m_info.sampleRate; }
    uint32 GetChannels() const { return m_info.channels; }
    uint64 GetSampleCount() const { return m_info.sampleCount; }

    // =========================================================================
    // Streaming
    // =========================================================================

    /**
     * @brief Enable streaming for large files
     */
    void SetStreaming(bool streaming) { m_streaming = streaming; }
    bool IsStreaming() const { return m_streaming; }

    // =========================================================================
    // Raw Data Access
    // =========================================================================

    /**
     * @brief Get raw PCM data (for non-streaming clips)
     */
    const void* GetRawData() const { return m_data.data(); }
    size_t GetRawDataSize() const { return m_data.size(); }

    // =========================================================================
    // Factory
    // =========================================================================

    static Ptr Create() { return std::make_shared<AudioClip>(); }
    static Ptr Create(const std::string& path);

private:
    std::string m_path;
    std::string m_name;
    AudioClipInfo m_info;
    bool m_loaded = false;
    bool m_streaming = false;

    std::vector<uint8> m_data;
    void* m_backendData = nullptr;  // miniaudio decoder/sound pointer
};

} // namespace RVX::Audio
