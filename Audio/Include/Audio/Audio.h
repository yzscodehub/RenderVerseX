/**
 * @file Audio.h
 * @brief Audio system unified header
 * 
 * Provides audio playback and 3D spatialization.
 * Primary backend: miniaudio (single-header, cross-platform)
 * 
 * Features:
 * - Audio playback (2D and 3D)
 * - Voice pool with priority-based stealing
 * - Bus-based mixing hierarchy
 * - DSP effects (reverb, filters, dynamics)
 * - Data-driven audio events
 * - Music system with crossfade and beat sync
 * - Audio streaming for large files
 * - Spatial audio zones
 */

#pragma once

// Core types and engine
#include "Audio/AudioTypes.h"
#include "Audio/AudioEngine.h"
#include "Audio/AudioSource.h"
#include "Audio/AudioListener.h"
#include "Audio/AudioClip.h"
#include "Audio/AudioSubsystem.h"

// Mixer
#include "Audio/Mixer/VoicePool.h"
#include "Audio/Mixer/AudioBus.h"
#include "Audio/Mixer/AudioMixer.h"

// DSP Effects
#include "Audio/DSP/IAudioEffect.h"
#include "Audio/DSP/ReverbEffect.h"
#include "Audio/DSP/LowPassEffect.h"
#include "Audio/DSP/DelayEffect.h"
#include "Audio/DSP/CompressorEffect.h"
#include "Audio/DSP/EffectChain.h"

// Events
#include "Audio/Events/AudioEvent.h"
#include "Audio/Events/AudioEventManager.h"

// Music
#include "Audio/Music/MusicTrack.h"
#include "Audio/Music/BeatSynchronizer.h"
#include "Audio/Music/MusicPlayer.h"

// Streaming
#include "Audio/Streaming/AudioStreamer.h"

// Spatial
#include "Audio/Spatial/AudioZone.h"
#include "Audio/Spatial/IOcclusionProvider.h"
#include "Audio/Spatial/AudioZoneManager.h"

namespace RVX::Audio
{

/**
 * @brief Audio system version info
 */
struct AudioSystemInfo
{
    static constexpr const char* kBackendName = "miniaudio";
    static constexpr int kMajorVersion = 1;
    static constexpr int kMinorVersion = 0;
    
    static constexpr const char* GetVersionString()
    {
        return "1.0.0";
    }
};

} // namespace RVX::Audio
