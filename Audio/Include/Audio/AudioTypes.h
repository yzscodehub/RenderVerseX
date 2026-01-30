/**
 * @file AudioTypes.h
 * @brief Core audio types and enumerations
 */

#pragma once

#include "Core/Types.h"
#include "Core/MathTypes.h"
#include <cstdint>

namespace RVX::Audio
{

/**
 * @brief Audio format
 */
enum class AudioFormat : uint8
{
    Unknown,
    U8,         ///< Unsigned 8-bit
    S16,        ///< Signed 16-bit
    S24,        ///< Signed 24-bit
    S32,        ///< Signed 32-bit
    F32         ///< 32-bit float
};

/**
 * @brief Audio channel layout
 */
enum class ChannelLayout : uint8
{
    Mono = 1,
    Stereo = 2,
    Surround51 = 6,
    Surround71 = 8
};

/**
 * @brief Sound attenuation model
 */
enum class AttenuationModel : uint8
{
    None,           ///< No distance attenuation
    Linear,         ///< Linear falloff
    Inverse,        ///< 1/distance falloff
    ExponentialDistance ///< Exponential falloff
};

/**
 * @brief Audio source state
 */
enum class AudioState : uint8
{
    Stopped,
    Playing,
    Paused
};

/**
 * @brief Audio playback settings
 */
struct AudioPlaySettings
{
    float volume = 1.0f;            ///< Volume (0-1)
    float pitch = 1.0f;             ///< Playback speed/pitch
    float pan = 0.0f;               ///< Stereo pan (-1 to 1)
    bool loop = false;              ///< Loop playback
    float fadeInTime = 0.0f;        ///< Fade in duration
    bool startPaused = false;       ///< Start paused
};

/**
 * @brief 3D audio settings
 */
struct Audio3DSettings
{
    Vec3 position{0.0f};
    Vec3 velocity{0.0f};
    Vec3 direction{0.0f, 0.0f, -1.0f};
    
    float minDistance = 1.0f;       ///< Distance where attenuation starts
    float maxDistance = 100.0f;     ///< Distance where sound is silent
    float rolloffFactor = 1.0f;     ///< Attenuation rolloff rate
    
    AttenuationModel attenuationModel = AttenuationModel::Inverse;
    
    float coneInnerAngle = 360.0f;  ///< Full volume cone angle (degrees)
    float coneOuterAngle = 360.0f;  ///< Zero volume cone angle
    float coneOuterGain = 0.0f;     ///< Volume outside outer cone
};

/**
 * @brief Audio clip information
 */
struct AudioClipInfo
{
    uint32 sampleRate = 44100;
    uint32 channels = 2;
    uint32 bitsPerSample = 16;
    uint64 sampleCount = 0;
    float duration = 0.0f;
    AudioFormat format = AudioFormat::S16;
};

/**
 * @brief Audio source handle
 */
class AudioHandle
{
public:
    AudioHandle() = default;
    explicit AudioHandle(uint64 id) : m_id(id) {}

    bool IsValid() const { return m_id != 0; }
    uint64 GetId() const { return m_id; }

    bool operator==(const AudioHandle& other) const { return m_id == other.m_id; }
    bool operator!=(const AudioHandle& other) const { return m_id != other.m_id; }

private:
    uint64 m_id = 0;
};

/**
 * @brief Reverb settings
 */
struct ReverbSettings
{
    float roomSize = 0.5f;
    float damping = 0.5f;
    float wetLevel = 0.3f;
    float dryLevel = 0.7f;
    float width = 1.0f;
};

/**
 * @brief Low-pass filter settings
 */
struct LowPassSettings
{
    float cutoffFrequency = 5000.0f;
    float resonance = 0.707f;
};

/**
 * @brief Audio bus for mixing
 */
struct AudioBus
{
    uint32 id = 0;
    float volume = 1.0f;
    float pan = 0.0f;
    bool muted = false;
    uint32 parentBus = 0;  // 0 = master
};

} // namespace RVX::Audio
