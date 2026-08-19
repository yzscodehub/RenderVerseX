#pragma once

/**
 * @file AudioFragments.h
 * @brief Data-only audio source, intent, and status fragments for the Scene ECS.
 *
 * Audio clips, playback engines, and runtime playback handles deliberately stay
 * outside these fragments. Audio bridges resolve the stable asset identity and
 * own transient generation-safe playback handles in side tables.
 */

#include "ECS/Fragment.h"
#include "RenderContracts/RenderIdentity.h"

namespace RVX::SceneECS
{
    /** @brief Backend-neutral distance attenuation policy. */
    enum class AudioAttenuationMode : uint8
    {
        None = 0,
        Linear,
        Inverse,
        ExponentialDistance,
    };

    /** @brief Explicit command carried by a monotonic audio intent sequence. */
    enum class AudioPlaybackCommand : uint8
    {
        None = 0,
        Play,
        Stop,
    };

    /** @brief Observable value-only result of an ECS audio binding. */
    enum class AudioBindingStatus : uint8
    {
        Unbound = 0,
        Active,
        Stopped,
        Inactive,
        PendingDestroy,
        InvalidConfiguration,
        PlaybackRejected,
        SynchronizationFailed,
    };

    /**
     * @brief Authored value configuration for one positional or non-positional sound.
     *
     * audioAssetId is a stable logical identity. It is never an AudioClip,
     * shared pointer, resource object, or engine-owned handle.
     */
    struct AudioEmitter
    {
        AssetId audioAssetId{};
        float volume = 1.0f;
        float pitch = 1.0f;
        float minDistance = 1.0f;
        float maxDistance = 100.0f;
        float rolloffFactor = 1.0f;
        float coneInnerAngleDegrees = 360.0f;
        float coneOuterAngleDegrees = 360.0f;
        float coneOuterGain = 0.0f;
        uint32 busId = 0;
        AudioAttenuationMode attenuation = AudioAttenuationMode::Inverse;
        bool loop = false;
        bool playOnStart = false;
        bool spatialize = true;
        /**
         * @brief Producer-owned revision for configuration diagnostics and restarts.
         *
         * Increment this for every value change, including positional fields
         * consumed only by Play3D. The bridge restarts on a revision change and
         * records a revision in AudioPlaybackState only after successful output.
         */
        uint64 configurationRevision = 1;
    };

    /**
     * @brief One value-only audio command.
     *
     * sequence must increase for a bridge to consume a new command. A command
     * is acknowledged in AudioPlaybackState only after its output effect has
     * succeeded (or Stop has proven the playback absent).
     */
    struct AudioPlaybackIntent
    {
        AudioPlaybackCommand command = AudioPlaybackCommand::None;
        uint64 sequence = 0;
    };

    /** @brief Runtime-visible audio bridge state; no playback object escapes here. */
    struct AudioPlaybackState
    {
        AudioBindingStatus status = AudioBindingStatus::Unbound;
        uint64 lastConsumedCommandSequence = 0;
        uint64 lastAppliedConfigurationRevision = 0;
        uint64 synchronizationRevision = 0;
    };

    static_assert(ECS::Fragment<AudioEmitter>);
    static_assert(ECS::Fragment<AudioPlaybackIntent>);
    static_assert(ECS::Fragment<AudioPlaybackState>);
} // namespace RVX::SceneECS
