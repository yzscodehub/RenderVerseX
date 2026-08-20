#pragma once

/**
 * @file AudioEcsBridge.h
 * @brief Pure ECS audio bridge and optional AudioEngine-backed gateway.
 */

#include "Audio/AudioClip.h"
#include "Audio/AudioEngine.h"
#include "Core/Handle.h"
#include "Scene/ECS/AudioFragments.h"
#include "Scene/ECS/SceneEcsRuntime.h"

#include <functional>
#include <memory>
#include <thread>
#include <vector>

namespace RVX::Audio
{
    class AudioSubsystem;

    /** @brief Generation-safe gateway-local playback identity. */
    struct AudioEcsPlaybackHandleTag {};
    using AudioEcsPlaybackHandle = Handle<AudioEcsPlaybackHandleTag>;

    /** @brief Immutable value request used to start one gateway playback. */
    struct AudioEcsPlayRequest
    {
        AssetId audioAssetId{};
        SceneECS::AudioEmitter emitter;
        Vec3 worldPosition{0.0f};
    };

    /** @brief Immutable value update for an already started gateway playback. */
    struct AudioEcsPlaybackUpdate
    {
        SceneECS::AudioEmitter emitter;
        Vec3 worldPosition{0.0f};
    };

    /** @brief Exact stop outcome used to preserve durable cleanup evidence. */
    enum class AudioEcsStopResult : uint8
    {
        Stopped = 0,
        AlreadyAbsent,
        Failed,
    };

    /**
     * @brief Bridge-facing playback service with only value inputs and handles.
     *
     * A gateway owns all engine pointers, decoded clips, and backend handles.
     * The bridge itself cannot observe or retain any of those objects.
     */
    class IAudioEcsPlaybackGateway
    {
    public:
        virtual ~IAudioEcsPlaybackGateway() = default;

        [[nodiscard]] virtual AudioEcsPlaybackHandle Play(const AudioEcsPlayRequest& request) = 0;
        [[nodiscard]] virtual bool Update(AudioEcsPlaybackHandle handle,
                                          const AudioEcsPlaybackUpdate& update) = 0;
        [[nodiscard]] virtual AudioEcsStopResult Stop(AudioEcsPlaybackHandle handle) = 0;
        [[nodiscard]] virtual bool IsPlaybackAlive(AudioEcsPlaybackHandle handle) const = 0;
    };

    /**
     * @brief AssetId-to-clip resolver used only inside the production gateway.
     *
     * The bridge supplies only an AssetId. The resolver's returned clip is
     * immediately consumed by AudioEngine and never stored in ECS fragments.
     */
    using AudioEcsClipResolver = std::function<AudioClip::Ptr(AssetId)>;

    /**
     * @brief AudioEngine-backed production gateway with a generation-safe side table.
     *
     * The referenced engine must outlive this gateway. It and every gateway
     * method must be used on the thread that constructed the gateway; calls
     * from other threads reject without touching AudioEngine state. The
     * optional AudioSubsystem overload simply binds its public GetEngine() API.
     */
    class AudioEcsEngineGateway final : public IAudioEcsPlaybackGateway
    {
    public:
        AudioEcsEngineGateway(AudioEngine& engine, AudioEcsClipResolver clipResolver);
        AudioEcsEngineGateway(AudioSubsystem& subsystem, AudioEcsClipResolver clipResolver);
        ~AudioEcsEngineGateway() override;

        AudioEcsEngineGateway(const AudioEcsEngineGateway&) = delete;
        AudioEcsEngineGateway& operator=(const AudioEcsEngineGateway&) = delete;
        AudioEcsEngineGateway(AudioEcsEngineGateway&&) = delete;
        AudioEcsEngineGateway& operator=(AudioEcsEngineGateway&&) = delete;

        [[nodiscard]] AudioEcsPlaybackHandle Play(const AudioEcsPlayRequest& request) override;
        [[nodiscard]] bool Update(AudioEcsPlaybackHandle handle,
                                  const AudioEcsPlaybackUpdate& update) override;
        [[nodiscard]] AudioEcsStopResult Stop(AudioEcsPlaybackHandle handle) override;
        [[nodiscard]] bool IsPlaybackAlive(AudioEcsPlaybackHandle handle) const override;

    private:
        struct Entry;

        [[nodiscard]] Entry* FindEntry(AudioEcsPlaybackHandle handle);
        [[nodiscard]] const Entry* FindEntry(AudioEcsPlaybackHandle handle) const;
        [[nodiscard]] bool IsOwnerThread() const;
        [[nodiscard]] bool Retire(AudioEcsPlaybackHandle handle);

        AudioEngine* m_engine = nullptr;
        AudioEcsClipResolver m_clipResolver;
        std::thread::id m_ownerThread;
        std::vector<Entry> m_entries;
        std::vector<uint32> m_freeEntries;
    };

    /** @brief Explicit bridge registration result; no legacy-object fallback exists. */
    enum class AudioEcsBridgeRegistrationResult : uint8
    {
        Registered = 0,
        AlreadyRegistered,
        InvalidRuntime,
        PlaybackGatewayUnavailable,
        ProcessorRegistrationFailed,
    };

    /** @brief Value diagnostic for one ECS entity-to-gateway association. */
    struct AudioEcsBindingDiagnostic
    {
        ECS::EntityHandle entity = ECS::EntityHandle::Invalid();
        AudioEcsPlaybackHandle playback = AudioEcsPlaybackHandle::Invalid();
        SceneECS::AudioBindingStatus status = SceneECS::AudioBindingStatus::Unbound;
        uint64 lastConsumedCommandSequence = 0;
        bool awaitingCleanupAcknowledgement = false;
    };

    /** @brief Deterministic bridge diagnostics ordered by EntityHandle. */
    struct AudioEcsBridgeDiagnosticsSnapshot
    {
        ECS::SceneRuntimeId sceneRuntimeId;
        AudioEcsBridgeRegistrationResult registration =
            AudioEcsBridgeRegistrationResult::InvalidRuntime;
        uint32 activePlaybackCount = 0;
        /** @brief Valid side-table handles still requiring a successful Stop/absence proof. */
        uint32 outstandingPlaybackCount = 0;
        uint32 pendingCleanupCount = 0;
        uint64 structuralContinuityLossCount = 0;
        uint64 cleanupContinuityLossCount = 0;
        uint64 authoritativeReconcileCount = 0;
        uint64 playCount = 0;
        uint64 stopCount = 0;
        uint64 rejectedConfigurationCount = 0;
        uint64 rejectedCommandCount = 0;
        uint64 synchronizationFailureCount = 0;
        uint64 playbackReleaseFailureCount = 0;
        uint64 shutdownPreparationCount = 0;
        uint64 shutdownStopFailureCount = 0;
        std::vector<AudioEcsBindingDiagnostic> bindings;
    };

    /**
     * @brief Owns pure-ECS audio playback state for exactly one Scene runtime.
     *
     * The bridge retains only EntityHandle values and gateway-local generational
     * handles. The referenced gateway must outlive the bridge. Processor
     * callbacks retain only a weak internal state and become inert after bridge
     * destruction. Before tearing down the gateway or its engine, the host must
     * call PrepareForShutdown() on the runtime owner thread and retry while
     * GetDiagnosticsSnapshot().outstandingPlaybackCount is non-zero. The
     * destructor is a last-resort best-effort Stop only.
     */
    class AudioEcsBridge
    {
    public:
        AudioEcsBridge(SceneECS::SceneEcsRuntime& runtime,
                       IAudioEcsPlaybackGateway& playbackGateway);
        ~AudioEcsBridge();

        AudioEcsBridge(const AudioEcsBridge&) = delete;
        AudioEcsBridge& operator=(const AudioEcsBridge&) = delete;
        AudioEcsBridge(AudioEcsBridge&&) = delete;
        AudioEcsBridge& operator=(AudioEcsBridge&&) = delete;

        [[nodiscard]] AudioEcsBridgeRegistrationResult RegisterProcessors();
        [[nodiscard]] bool IsRegistered() const;
        [[nodiscard]] ECS::SceneRuntimeId GetSceneRuntimeId() const;

        /**
         * @brief Attempt to drain every bridge-owned playback before host teardown.
         * @return True only when every live side-table handle proved stopped or absent.
         */
        [[nodiscard]] bool PrepareForShutdown();

        /** @brief Return a handle only for this runtime and an exact live entity generation. */
        [[nodiscard]] AudioEcsPlaybackHandle FindPlayback(
            ECS::SceneRuntimeId sceneRuntimeId,
            ECS::EntityHandle entity) const;
        [[nodiscard]] AudioEcsBridgeDiagnosticsSnapshot GetDiagnosticsSnapshot() const;

    private:
        struct State;
        std::shared_ptr<State> m_state;
    };
} // namespace RVX::Audio
